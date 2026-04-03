#!/usr/bin/env zsh

source /opt/toolchains/dc/kos/environ.sh

PATH=$PATH:/opt/toolchains/dc/bin

mkdccdi () {
   mkdir -p cdrom
   mkdcdisc -n $1 -e $1.elf -d cdrom/ -N -o $1.cdi -v 3 -m
}

kosstyle () {
	astyle --style=attach -s4 -c -S -w -y -f -U -p -m0 -xn -xc -xl -xk -C -N -Y \
    -k3 -xL -xM -xQ -xP0 $1
}

export DCTOOL_HOST=10.0.0.248
export PFX_HOST=10.0.0.244

# Description:
# The ensure_dctool_ready function safely manages Dreamcast system readiness for
# development and testing, especially with PixelFX Retro Gem HDMI mods or similar
# setups. It performs the following steps:
#
# 1. Prompts for and exports required environment variables ($DCTOOL_HOST,
#    $PFX_USER, $PFX_PASSWD, $PFX_HOST) if not already set.
# 2. Checks if $DCTOOL_HOST is online by pinging it. If online, waits for a
#    successful ping response and skips the restart.
# 3. If $DCTOOL_HOST is offline, sends a POST request to the system-restart
#    endpoint on $PFX_HOST using the provided credentials to trigger a hardware
#    restart.
# 4. Waits up to 60 seconds for $DCTOOL_HOST to respond to ping, ensuring the
#    Dreamcast is ready for further operations.
# 5. Returns success if the Dreamcast is online, or failure if it does not
#    respond within the timeout.
function ensure_dctool_ready(){
  if [[ -z "$DCTOOL_HOST" ]]; then
    echo -n "Enter DCTOOL_HOST IP OR HOSTNAME: "
    read DCTOOL_HOST
    export DCTOOL_HOST
  fi
  echo "Checking if DCTOOL_HOST ($DCTOOL_HOST) is online..."
  timeout=6
  elapsed=0
  while ! ping -c 1 -W 1 "$DCTOOL_HOST" >/dev/null 2>&1; do
    ((++elapsed))
    echo "Waiting for DCTOOL_HOST ($DCTOOL_HOST) to respond to ping for $timeout seconds... ($elapsed seconds elapsed)"
    if [[ $elapsed -ge $timeout ]]; then
      echo "DCTOOL_HOST ($DCTOOL_HOST) did not respond to ping within $timeout seconds. Assuming it is offline."
      break
    fi
    sleep 1
  done
  if ping -c 1 -W 1 "$DCTOOL_HOST" >/dev/null 2>&1; then
    echo "DCTOOL_HOST ($DCTOOL_HOST) is online. Skipping system restart."
    return 0
  fi
 
  if [[ -z "$PFX_USER" ]]; then
    echo -n "Enter pixel fx username: "
    read PFX_USER
    export PFX_USER
  fi
  if [[ -z "$PFX_PASSWD" ]]; then
    echo -n "Enter pixel fx password: "
    read -s PFX_PASSWD
    echo
    export PFX_PASSWD
  fi
  if [[ -z "$PFX_HOST" ]]; then
    echo -n "Enter host (IP OR HOSTNAME, e.g. 10.0.0.244): "
    read PFX_HOST
    export PFX_HOST
  fi
  curl -v --digest -u "$PFX_USER:$PFX_PASSWD" -X POST "http://$PFX_HOST/system-restart"
  echo "Waiting for DCTOOL_HOST ($DCTOOL_HOST) to respond to ping after system restart..."
  timeout=30
  elapsed=0
  while ! ping -c 1 -W 1 "$DCTOOL_HOST" >/dev/null 2>&1; do
    echo "Waiting for DCTOOL_HOST ($DCTOOL_HOST) to respond to ping for $timeout seconds... ($elapsed seconds elapsed)"
    sleep 1
    ((++elapsed))
    if [[ $elapsed -ge $timeout ]]; then
      echo "Timeout: DCTOOL_HOST did not respond to ping within $timeout seconds after restart."
      return 1
    fi
  done
  echo "DCTOOL_HOST ($DCTOOL_HOST) responded to ping after restart."
  return 0
}
