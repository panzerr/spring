#!/bin/bash

SCRIPT_DIR=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )" &> /dev/null && pwd )
DATA_DIR="$SCRIPT_DIR/../_data"
DATA_FILE="$DATA_DIR/latest_release.json"
WORK_DIR="$SCRIPT_DIR/tmp"
CONFIG_FILE="$DATA_DIR/configs.json"
WDEFS_FILE="$DATA_DIR/weapondefs.json"

DOWNLOAD_URL=$(jq -r '.assets[] | select(.name | contains("_linux-64-minimal-portable")).browser_download_url' $DATA_FILE)

echo "> downloading latest engine release from $DOWNLOAD_URL"

mkdir $WORK_DIR
cd $WORK_DIR

curl -L $DOWNLOAD_URL -o engine.7z
7z -y e engine.7z spring

echo "> writing $CONFIG_FILE"
rm -f $CONFIG_FILE
./spring --list-config-vars | grep -v "^\[t=" > $CONFIG_FILE

echo "> writing $WDEFS_FILE"
rm -f $WDEFS_FILE
./spring --list-def-tags | grep -v "^\[t=" > $WDEFS_FILE
