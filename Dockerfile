FROM ubuntu:xenial

ENV DEBIAN_FRONTEND noninteractive

RUN apt-get update
RUN apt-get install -y openssl

COPY ./ratelimit /ratelimit/ratelimit
RUN chmod a+x /ratelimit/ratelimit

VOLUME /data
EXPOSE 9049

CMD ["/ratelimit/ratelimit", "--rocksdb_db_path", "/data/ratelimit", "--port", "9049", "--rocksdb_create_if_missing"]
