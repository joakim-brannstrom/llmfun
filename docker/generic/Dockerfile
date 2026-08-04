FROM alpine:latest
RUN apk add --no-cache git bash \
    && git config --global --add safe.directory /workarea \
    && git config --global user.email "agent@example.com" \
    && git config --global user.name "Coding Agent"

RUN mkdir /workarea
