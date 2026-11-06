#!/bin/sh
# Policy plumbing test using temporarily lowered passive trips and REAL sensors.
# This is not physical high-temperature characterization. Critical stays 110C.
set -eu
D=/sys/devices/platform/soc/soc:mm/23140000.gpu
F=/sys/class/devfreq/23140000.gpu
L=/run/m21-share
Z=
for z in /sys/class/thermal/thermal_zone*; do
 [ "$(cat "$z/type")" != soc-thmzone ] || Z=$z
done
test -n "$Z"
test "$(cat "$Z/cdev0/type")" = devfreq-23140000.gpu
test "$(cat "$Z/cdev0_trip_point")" = 1
test "$(cat "$Z/trip_point_2_temp")" = 110000
original0=$(cat "$Z/trip_point_0_temp")
original1=$(cat "$Z/trip_point_1_temp")
original_governor=$(cat "$F/governor")
original_control=$(cat "$D/power/control")
restore()
{
 echo "$original1" > "$Z/trip_point_1_temp"
 echo "$original0" > "$Z/trip_point_0_temp"
 echo "$original_governor" > "$F/governor"
 echo "$original_control" > "$D/power/control"
 rmmod w202_gpu_frequency 2>/dev/null || true
}
trap restore EXIT
trap 'exit 1' HUP INT TERM
wait_state()
{
 n=0
 while [ "$(cat "$Z/cdev0/cur_state")" != "$1" ] && [ "$n" -lt 50 ]; do
  sleep 0.1
  n=$((n+1))
 done
 test "$(cat "$Z/cdev0/cur_state")" = "$1"
}
measure()
{
 reading=$(cat "$D/w202_gpu_frequency")
 echo "$reading"
 hz=${reading#measured_hz=}
 hz=${hz%% *}
 test "$hz" -ge "$1"
 test "$hz" -le "$2"
}
exec > "$L/thermal-policy.log" 2>&1
echo TEST_USES_REAL_TEMPERATURE_AND_TEMPORARILY_LOWERED_PASSIVE_TRIPS
echo performance > "$F/governor"
echo on > "$D/power/control"
insmod /lib/modules/w202_gpu_frequency.ko
wait_state 0
measure 380000000 388000000
for round in 1 2 3; do
 echo ROUND="$round"
 temp=$(cat "$Z/temp")
 test "$temp" -gt 25000
 test "$temp" -lt 65000
 echo 10000 > "$Z/trip_point_0_temp"
 echo 20000 > "$Z/trip_point_1_temp"
 wait_state 1
 echo CAPPED temperature="$temp" state="$(cat "$Z/cdev0/cur_state")"
 measure 152000000 155000000
 /bin/gpu-kms-test 30 > "$L/thermal-capped-$round-kms.log" 2>&1
 grep GPU_DMA_BUF_DPU_SCANOUT_PASS "$L/thermal-capped-$round-kms.log"
 echo "$original1" > "$Z/trip_point_1_temp"
 echo "$original0" > "$Z/trip_point_0_temp"
 wait_state 0
 echo RELEASED state="$(cat "$Z/cdev0/cur_state")"
 measure 380000000 388000000
done
test "$(cat "$Z/trip_point_2_temp")" = 110000
echo M21_THERMAL_POLICY_LIMIT_AND_RELEASE_PASS
