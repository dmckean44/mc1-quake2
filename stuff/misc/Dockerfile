FROM ubuntu:18.04 AS build

WORKDIR /mc1quake2
RUN apt-get update
RUN apt-get install libcurl4-openssl-dev build-essential -y
COPY . .

RUN make server game

FROM ubuntu:18.04 AS runtime

ENV server_cfg="server.cfg"

EXPOSE 27910

# Add the user UID:1000, GID:1000, home at /mc1quake2
RUN groupadd -r mc1quake2 -g 1000 && useradd -u 1000 -r -g mc1quake2 -m -d /mc1quake2 -s /sbin/nologin -c "mc1quake2 user" mc1quake2 && \
    chmod 755 /mc1quake2

WORKDIR /mc1quake2
COPY --from=build /mc1quake2/release ./

# Specify the user to execute all commands below
USER mc1quake2

RUN mkdir .mc1q2
VOLUME ["/mc1quake2/.mc1q2"]
ENTRYPOINT ./q2ded +exec ${server_cfg}
