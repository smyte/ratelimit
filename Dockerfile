FROM ubuntu:xenial

ENV DEBIAN_FRONTEND noninteractive

RUN apt-get update
RUN apt-get install -y openssl

COPY ./ratelimit /smyte/ratelimit
RUN chmod a+x /smyte/ratelimit

VOLUME /data

EXPOSE 9008
CMD ["/smyte/ratelimit", "--rocksdb_db_path", "/data/ratelimit", "--port", "9008", "--rocksdb_create_if_missing_one_off", "--version_timestamp_ms", "2461796973728"]