#!/bin/sh
set -eu
install -d -m 0750 -o root -g mosquitto /etc/mosquitto/certs
install -m 0640 -o root -g mosquitto "${RENEWED_LINEAGE}/fullchain.pem" /etc/mosquitto/certs/fullchain.pem
install -m 0640 -o root -g mosquitto "${RENEWED_LINEAGE}/privkey.pem" /etc/mosquitto/certs/privkey.pem
systemctl reload mosquitto
