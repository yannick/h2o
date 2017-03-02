/*
 * Copyright (c) 2015-2016 DeNA Co., Ltd., Kazuho Oku
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include <mruby.h>
#include <mruby/array.h>
#include <mruby/error.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <mruby_input_stream.h>
#include "h2o/mruby_.h"
#include "embedded.c.h"

#include "h2o/socket.h"

struct st_h2o_mruby_socket_context_t {
    h2o_mruby_context_t *ctx;
    h2o_socket_t *sock;
    size_t bytes_written;
    size_t read_length;
//    h2o_timeout_entry_t _timeout;
    h2o_hostinfo_getaddr_req_t *_getaddr_req;
    mrb_value receiver;
    struct {
        mrb_value socket;
    } refs;
};

static void on_gc_dispose_tcp_socket(mrb_state *mrb, void *_ctx)
{
    struct st_h2o_mruby_socket_context_t *ctx = _ctx;
    if (ctx == NULL) return;
    ctx->refs.socket = mrb_nil_value();
}

const static struct mrb_data_type tcp_socket_type = {"tcp_socket", on_gc_dispose_tcp_socket};

static void attach_receiver(struct st_h2o_mruby_socket_context_t *ctx, mrb_value receiver)
{
    assert(mrb_nil_p(ctx->receiver));
    ctx->receiver = receiver;
    mrb_gc_register(ctx->ctx->shared->mrb, receiver);
}

static mrb_value detach_receiver(struct st_h2o_mruby_socket_context_t *ctx)
{
    mrb_value ret = ctx->receiver;
    assert(!mrb_nil_p(ret));
    ctx->receiver = mrb_nil_value();
    mrb_gc_unregister(ctx->ctx->shared->mrb, ret);
    mrb_gc_protect(ctx->ctx->shared->mrb, ret);
    return ret;
}

static void dispose_context(struct st_h2o_mruby_socket_context_t *ctx)
{
    if (ctx->sock != NULL) {
        h2o_socket_close(ctx->sock);
        ctx->sock = NULL;
    }

    if (!mrb_nil_p(ctx->refs.socket))
        DATA_PTR(ctx->refs.socket) = NULL;

    free(ctx);
}

static void on_connect_error(struct st_h2o_mruby_socket_context_t *ctx, const char *errstr)
{
    mrb_state *mrb = ctx->ctx->shared->mrb;
    assert(errstr != NULL);

    int gc_arena = mrb_gc_arena_save(mrb);

    mrb_value receiver = detach_receiver(ctx);
    mrb_value exc = mrb_exc_new(mrb, E_RUNTIME_ERROR, errstr, strlen(errstr)); // TODO: SocketError
    h2o_mruby_run_fiber(ctx->ctx, receiver, exc, NULL);

    mrb_gc_arena_restore(mrb, gc_arena);

    dispose_context(ctx);
}

static void on_connect(h2o_socket_t *sock, const char *errstr)
{
    struct st_h2o_mruby_socket_context_t *ctx = sock->data;
    assert(ctx->sock == sock);

    if (errstr != NULL) {
        on_connect_error(ctx, errstr);
        return;
    }

    mrb_state *mrb = ctx->ctx->shared->mrb;

    int gc_arena = mrb_gc_arena_save(mrb);

    mrb_value receiver = detach_receiver(ctx);
    h2o_mruby_run_fiber(ctx->ctx, receiver, ctx->refs.socket, NULL);

    mrb_gc_arena_restore(mrb, gc_arena);
}

static void start_connect(struct st_h2o_mruby_socket_context_t *ctx, struct sockaddr *addr, socklen_t addrlen)
{
    if ((ctx->sock = h2o_socket_connect(ctx->ctx->shared->ctx->loop, addr, addrlen, on_connect)) == NULL) {
        on_connect_error(ctx, "socket create error");
        return;
    }
    ctx->sock->data = ctx;
}

static void on_getaddr(h2o_hostinfo_getaddr_req_t *getaddr_req, const char *errstr, struct addrinfo *res, void *_ctx)
{
    struct st_h2o_mruby_socket_context_t *ctx = _ctx;

    assert(getaddr_req == ctx->_getaddr_req);
    ctx->_getaddr_req = NULL;

    if (errstr != NULL) {
        on_connect_error(ctx, errstr);
        return;
    }

    /* start connecting */
    struct addrinfo *selected = h2o_hostinfo_select_one(res);
    start_connect(ctx, selected->ai_addr, selected->ai_addrlen);
}


static struct st_h2o_mruby_socket_context_t *create_socket(h2o_mruby_context_t *mctx, mrb_value _host, mrb_value _service)
{
    mrb_state *mrb = mctx->shared->mrb;
    h2o_iovec_t host = h2o_iovec_init(RSTRING_PTR(_host), RSTRING_LEN(_host));

    h2o_iovec_t service;
    if (mrb_fixnum_p(_service)) _service = mrb_funcall(mrb, _service, "to_s", 0);
    if (mrb_string_p(_service)) {
        service = h2o_iovec_init(RSTRING_PTR(_service), RSTRING_LEN(_service));
    } else {
        mrb_raise(mrb, E_ARGUMENT_ERROR, "service must be string or fixnum");
        return NULL;
    }

    h2o_mruby_shared_context_t *shared = mrb->ud;
    struct st_h2o_mruby_socket_context_t *ctx;

    ctx = h2o_mem_alloc(sizeof(*ctx));
    ctx->ctx = shared->current_context;
    ctx->receiver = mrb_nil_value();
    ctx->bytes_written = 0;
    ctx->read_length = 0;

    ctx->refs.socket = h2o_mruby_create_data_instance(mrb, mrb_ary_entry(ctx->ctx->shared->constants, H2O_MRUBY_TCP_SOCKET_CLASS), ctx, &tcp_socket_type);

    if (mrb_fixnum_p(_service)) {
        /* directly call connect(2) if `host` is an IP address and `service` is fixnum */
        mrb_int port = mrb_int(mrb, _service);

        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        if (h2o_hostinfo_aton(host, &sin.sin_addr) == 0) {
            sin.sin_family = AF_INET;
            sin.sin_port = htons(port);
            start_connect(ctx, (void *)&sin, sizeof(sin));
            return ctx;;
        }
    }

    /* resolve destination and then connect */
    ctx->_getaddr_req = h2o_hostinfo_getaddr(&ctx->ctx->shared->ctx->receivers.hostinfo_getaddr, host, service, AF_UNSPEC,
                         SOCK_STREAM, IPPROTO_TCP, AI_ADDRCONFIG | AI_NUMERICSERV, on_getaddr, ctx);

    return ctx;
}


mrb_value h2o_mruby_socket_connect_callback(h2o_mruby_context_t *mctx, mrb_value receiver, mrb_value args, int *run_again)
{
    mrb_state *mrb = mctx->shared->mrb;

    mrb_value host = mrb_ary_entry(args, 0);
    mrb_value service = mrb_ary_entry(args, 1);

    struct st_h2o_mruby_socket_context_t *ctx = create_socket(mctx, host, service);
    if (mrb->exc != NULL) {
        *run_again = 1;
        return mrb_obj_value(mrb->exc);
    }
    assert(ctx != NULL);

    attach_receiver(ctx, receiver);
    return mrb_nil_value();
}

static void on_write_complete(h2o_socket_t *sock, const char *errstr)
{
    struct st_h2o_mruby_socket_context_t *ctx = sock->data;
    mrb_state *mrb = ctx->ctx->shared->mrb;

    if (errstr != NULL) {
        // TODO
        return;
    }

    size_t written = sock->bytes_written - ctx->bytes_written;
    ctx->bytes_written = sock->bytes_written;

    int gc_arena = mrb_gc_arena_save(mrb);

    mrb_value receiver = detach_receiver(ctx);

    h2o_mruby_run_fiber(ctx->ctx, receiver, mrb_fixnum_value(written), NULL);

    mrb_gc_arena_restore(mrb, gc_arena);
}

mrb_value h2o_mruby_socket_write_callback(h2o_mruby_context_t *mctx, mrb_value receiver, mrb_value args, int *run_again)
{
    mrb_state *mrb = mctx->shared->mrb;
    struct st_h2o_mruby_socket_context_t *ctx;

    if ((ctx = mrb_data_check_get_ptr(mrb, mrb_ary_entry(args, 0), &tcp_socket_type)) == NULL) {
        *run_again = 1;
        return mrb_exc_new_str_lit(mrb, E_ARGUMENT_ERROR, "TCPSocket#write wrong self");
    }

    mrb_value str = mrb_ary_entry(args, 1);
    if (! mrb_string_p(str)) {
        str = mrb_funcall(mrb, str, "to_s", 0);
    }

    h2o_iovec_t bufs[1];
    bufs[0] = h2o_iovec_init(RSTRING_PTR(str), RSTRING_LEN(str));
    h2o_socket_write(ctx->sock, bufs, 1, on_write_complete);

    attach_receiver(ctx, receiver);
    return mrb_nil_value();
}


static void on_read(h2o_socket_t *sock, const char *errstr)
{
    struct st_h2o_mruby_socket_context_t *ctx = sock->data;
    mrb_state *mrb = ctx->ctx->shared->mrb;

    if (errstr != NULL) {
        if (sock->bytes_read > 0) {
            // TODO
            return;
        } else {
            // EOF
            
        }

    }

    size_t consume_length;
    // TODO: handle eof
    if (ctx->read_length == SIZE_MAX) {
        if (sock->bytes_read > 0) {
            return;
        }
        consume_length = sock->input->size;
    } else {
        if (sock->input->size < ctx->read_length) {
            return;
        }
        consume_length = ctx->read_length;
    }

    int gc_arena = mrb_gc_arena_save(mrb);

    mrb_value read = mrb_str_new(ctx->ctx->shared->mrb, sock->input->bytes, consume_length);
    h2o_buffer_consume(&sock->input, consume_length);
    h2o_socket_read_stop(sock);

    mrb_value receiver = detach_receiver(ctx);

    h2o_mruby_run_fiber(ctx->ctx, receiver, read, NULL);

    mrb_gc_arena_restore(mrb, gc_arena);

    ctx->read_length = 0;
}

mrb_value h2o_mruby_socket_read_callback(h2o_mruby_context_t *mctx, mrb_value receiver, mrb_value args, int *run_again)
{
    mrb_state *mrb = mctx->shared->mrb;
    struct st_h2o_mruby_socket_context_t *ctx;

    if ((ctx = mrb_data_check_get_ptr(mrb, mrb_ary_entry(args, 0), &tcp_socket_type)) == NULL) {
        *run_again = 1;
        return mrb_exc_new_str_lit(mrb, E_ARGUMENT_ERROR, "TCPSocket#read wrong self");
    }

    assert(ctx->read_length == 0);
    mrb_value length = mrb_ary_entry(args, 1);
    if (mrb_nil_p(length)) {
        ctx->read_length = SIZE_MAX; // means read whole data until EOF
    } else {
        ctx->read_length = mrb_int(mrb, length);
    }

    h2o_socket_read_start(ctx->sock, on_read);

    attach_receiver(ctx, receiver);
    return mrb_nil_value();
}

static mrb_value close_method(mrb_state *mrb, mrb_value self)
{
    assert(!"FIXME");
    return mrb_nil_value();
}

void h2o_mruby_socket_init_context(h2o_mruby_shared_context_t *ctx)
{
    mrb_state *mrb = ctx->mrb;

    h2o_mruby_eval_expr(mrb, H2O_MRUBY_CODE_SOCKET);
    h2o_mruby_assert(mrb);


    struct RClass *module = mrb_module_get(mrb, "H2O");
    struct RClass *klass = mrb_class_get_under(mrb, module, "TCPSocket");
    mrb_ary_set(mrb, ctx->constants, H2O_MRUBY_TCP_SOCKET_CLASS, mrb_obj_value(klass));

    mrb_define_method(mrb, klass, "close", close_method, MRB_ARGS_NONE());

    h2o_mruby_define_callback(mrb, "_h2o_socket_connect", H2O_MRUBY_CALLBACK_ID_SOCKET_CONNECT);
    h2o_mruby_define_callback(mrb, "_h2o_socket_write", H2O_MRUBY_CALLBACK_ID_SOCKET_WRITE);
    h2o_mruby_define_callback(mrb, "_h2o_socket_read", H2O_MRUBY_CALLBACK_ID_SOCKET_READ);

//    klass = mrb_class_get_under(mrb, module, "HttpInputStream");
//    mrb_ary_set(mrb, ctx->constants, H2O_MRUBY_HTTP_INPUT_STREAM_CLASS, mrb_obj_value(klass));
//
//    h2o_mruby_define_callback(mrb, "_h2o__http_join_response", H2O_MRUBY_CALLBACK_ID_HTTP_JOIN_RESPONSE);
//    h2o_mruby_define_callback(mrb, "_h2o__http_fetch_chunk", H2O_MRUBY_CALLBACK_ID_HTTP_FETCH_CHUNK);
}
