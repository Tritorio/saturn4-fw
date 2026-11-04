# saturn4-fw mod 8.3a: chitu runs with the passive serial tap (libsgtap.so) unless
# /customer/mod/sgtap_disable exists (kill switch, create it via telnet + reboot)
# 8.24: boot guard for the saved machine settings (puts the last good backup back if the file is invalid);
# waits at most 3 s so that it can never hold the app back
if [ -f /customer/mod/machguard.tcl ] && [ ! -f /customer/mod/machguard_disable ]; then
    jimsh /customer/mod/machguard.tcl >/dev/null 2>&1 &
    gpid=$!
    n=0
    while kill -0 $gpid 2>/dev/null && [ $n -lt 15 ]; do sleep 0.2; n=$((n + 1)); done
fi
if [ -f /customer/mod/libsgtap.so ] && [ ! -f /customer/mod/sgtap_disable ]; then
    LD_PRELOAD=/customer/mod/libsgtap.so /customer/resources/chitu&
else
    /customer/resources/chitu&
fi
# saturn4-fw mod: hook runs after the app, a failing hook cannot stop the app
/customer/mod/hook.sh &
