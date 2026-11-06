#!/bin/sh
# SPDX-License-Identifier: MIT
# Bounded Linux test against the previously verified CAICT Yunfu Mobile node.
# Does not install or emulate the official app. SOURCE must be cellular IPv4.
set -eu
src=${1:?usage: caict-speed.sh SOURCE_IPV4 NEW_OUTPUT_DIRECTORY}
work=${2:?provide a fresh absolute output directory}
case "$work" in /*) ;; *) exit 2 ;; esac
case "$src" in *[!0-9.]*|'') exit 2 ;; esac
node=${CAICT_NODE:-120.240.33.102}
port=${CAICT_PORT:-9443}
case "$node:$port" in *[!0-9.:]*|'') exit 2 ;; esac
base=http://$node:$port
# The node's download and multipart upload handlers use these protocol clients.
ua_down='Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/100.0.4896.60 Safari/537.36'
ua_up='Dalvik/1.6.0 (Linux; U; Android 4.2.2; GT-I9505 Build/JDQ39)'
umask 077
mkdir "$work"
ip -4 route get "$node" from "$src" > "$work/route.txt"
grep -q 'dev sipa_eth0' "$work/route.txt"
lease= monitor= pids=
cleanup() {
    for pid in $pids; do kill "$pid" 2>/dev/null || :; done
    if test -n "$monitor"; then kill "$monitor" 2>/dev/null || :; wait "$monitor" 2>/dev/null || :; fi
    if test -n "$lease"; then
        curl -q --noproxy '*' -4 --interface "$src" --connect-timeout 3 --max-time 5 \
            --max-filesize 65536 -fsS -X POST "$base/speed/dovalid?key=$lease" \
            > "$work/dequeue.txt" 2> "$work/dequeue.err" || :
    fi
    rm -f "$work/upload.bin"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP
# Protocol from the previously inspected TaierSpeedtest client. Use a fresh
# synthetic client identifier, never the SIM or tablet's IMEI.
stamp=$(date +%s)
client=$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')
part1=$(printf '%s' "model=Android&imei=$client" | md5sum | cut -d ' ' -f1)
part2=$(printf '%s' "stime=$stamp&band=1000&rand=12345555" | md5sum | cut -d ' ' -f1)
token=$(printf '%s' "$part1$part2" | md5sum | cut -d ' ' -f1)
lease=$(curl -q --noproxy '*' -4 --interface "$src" --connect-timeout 5 --max-time 10 \
    --max-filesize 65536 --user-agent "$ua_up" -fsS "$base/speed/dovalid?key=&flag=true&bandwidth=1000&model=Android&imei=$client&time=$stamp&app=globalspeed&token=$token&pkg=com.cnspeedtest.globalspeed")
case "$lease" in
    1-????????-????-????-????-????????????) lease=${lease#1-} ;;
    0*) echo NODE_BUSY; lease=; exit 1 ;;
    2*) echo NODE_QUEUE_PENDING; lease=; exit 1 ;;
    *) printf '%s\n' "$lease" > "$work/lease-rejection.txt"; echo NODE_DID_NOT_GRANT_LEASE; lease=; exit 1 ;;
esac
printf 'NODE=%s:%s SOURCE=%s INTERFACE=sipa_eth0\n' "$node" "$port" "$src"
ping -n -I "$src" -c 5 -W 1 "$node" > "$work/ping.txt" || :
(
    while :; do
        printf '%s %s %s\n' "$(date +%s.%N)" \
            "$(cat /sys/class/net/sipa_eth0/statistics/rx_bytes)" \
            "$(cat /sys/class/net/sipa_eth0/statistics/tx_bytes)"
        sleep 0.5
    done
) > "$work/counters.txt" & monitor=$!
date +%s.%N > "$work/download-start.txt"
for connection in 1 2 3 4 5 6 7 8; do
    curl -q --noproxy '*' -4 --interface "$src" --connect-timeout 5 --max-time 20 \
        --user-agent "$ua_down" \
        --max-filesize 33554432 --range 0-33554431 --header 'Connection: close' \
        --header "Key: $lease" -fsS -o /dev/null \
        -w '{"curl_exit":%{exitcode},"http":%{http_code},"bytes":%{size_download},"seconds":%{time_total},"source":"%{local_ip}","remote":"%{remote_ip}"}\n' \
        "$base/speed/File(1G).dl?r=$(date +%s)&key=$lease" \
        > "$work/download-$connection.json" 2> "$work/download-$connection.err" &
    pids="$pids $!"
done
for pid in $pids; do wait "$pid" || :; done
pids=
date +%s.%N > "$work/download-end.txt"
dd if=/dev/urandom of="$work/upload.bin" bs=1048576 count=4 status=none
date +%s.%N > "$work/upload-start.txt"
for connection in 1 2 3 4; do
    curl -q --noproxy '*' -4 --interface "$src" --connect-timeout 5 --max-time 25 \
        --user-agent "$ua_up" \
        --max-filesize 65536 --header "Key: $lease" \
        --form "upload=@$work/upload.bin;filename=SPEED_test.bin;type=application/octet-stream" \
        -fsS -o /dev/null \
        -w '{"curl_exit":%{exitcode},"http":%{http_code},"bytes":%{size_upload},"seconds":%{time_total},"source":"%{local_ip}","remote":"%{remote_ip}"}\n' \
        "$base/speed/doAnalsLoad.do" > "$work/upload-$connection.json" 2> "$work/upload-$connection.err" &
    pids="$pids $!"
done
for pid in $pids; do wait "$pid" || :; done
pids=
date +%s.%N > "$work/upload-end.txt"
ip -s link show sipa_eth0 > "$work/link.txt"
cat "$work/ping.txt"
echo CAICT_TRANSFER_COMPLETE_CHECK_JSON_RESULTS
