### Kafka plugin for h2o

[librdkafka](https://github.com/edenhill/librdkafka) is used to send log messages to a kafka server.

#### Build H2O without librdkafka
`-DWITH_RDKAFKA=off` option can be added to cmake to exclude `librdkafka` from  h2o configuration.
For example:

```sh
cmake -DWITH_BUNDLED_SSL=on -DWITH_RDKAFKA=off .
```

#### Configure Kafka

`kafka-log` is a host option for kafka configuration.
Each host (path) may have one `kafka-log` option.
Each `kafka-log` configuration may have multiple `topic` sub-configurations.

`kafka-log` and `topic` configurations allow to add all appropriate options from [the rdkafka configuration list](https://github.com/edenhill/librdkafka/blob/master/CONFIGURATION.md).

##### Configure Kafka Topic

`topic` has following special options:
  1. `name` - topic name (required)
  2. `message` - h2o format string for message (optional)
  3. `key` - h2o format string for message key (optional, default is no key)
  4. `partition_hash` - h2o format string for string to hash it using CRC32 hash and compute partition number (optional). By default Kafka uses `key` value if any to hash it and compute partition.

##### Example
```yml
# to find out the configuration commands, run: h2o --help

listen: 8080
hosts:
  "127.0.0.1.xip.io:8080":
    paths:
      /:
        file.dir: examples/doc_root
    access-log: /dev/stdout
    kafka-log:
      metadata.broker.list: localhost
      group.id: rdkafkad
      api.version.request: true
      <... other_kafka_configs>
      topic:
        name: h2o-kafka <required>
        message: <optional_format_string>
        key: <optional_format_string>
        partition_hash: <optional_format_string>
        # <... other_kafka_topic_configs>
      topic:
        name: h2o-kafka-2 <required>
        message: <optional_format_string>
        key: <optional_format_string>
        partition_hash: <optional_format_string>
        # <... other_kafka_topic_configs>
```
