#!/bin/bash

testlist=$(cat $TEST_LIST | grep '^[^#]\+$')
crit_test_fail=0

for test_dir in $testlist; do
    test="$test_dir/runtest.sh"
    testname="$(grep 'TEST=\".*\"' $test | awk -F '=' '{ print $2 }' | sed 's/"//g')"
    short_testname=${testname:0:55}
    outdir="$OUTPUT_ROOT/test/$testname"
    logfile="$outdir/full.log"

    mkdir -p $outdir

    syslog $short_testname
    $RUNNER_SCRIPT $test &> $logfile
    syslog "End: $short_testname"

    # save post crashes
    if [ -d /var/spool/abrt ]; then
        n_post=$( find /var/spool/abrt/ -type d | wc -l )
        if [ $n_post -gt 0 ]; then
            mkdir "$outdir/post_crashes"
            for dir in $( find /var/spool/abrt/ -type d ); do
                mv "$dir" "$outdir/post_crashes/"
            done
        fi
    fi

    # extract test protocol
    start_line=$(grep -n -i 'Test protocol' $logfile | awk -F: '{print $1}')
    end_line=$(grep -n -i 'TEST END MARK' $logfile | awk -F: '{print $1}')

    if [ $start_line -gt 0 ]; then
        start_line=$[ $start_line - 1 ]
    fi
    end_line=$[ $end_line - 1 ]

    sed -n "${start_line},${end_line}p;${end_line}q" $logfile \
        > "$outdir/protocol.log"

    # collect /var/log/messages
    start=$( grep -n "MARK: $short_testname.*" '/var/log/messages'  | tail -n 1 | awk -F: '{print $1}' )
    end=$( grep -n "MARK: End: $short_testname.*" '/var/log/messages'  | tail -n 1 | awk -F: '{print $1}' )
    start=$[ $start + 2 ]
    end=$[ $end - 2 ]

    sed -n "${start},${end}p;${end}q" '/var/log/messages' > "$outdir/messages"

    # collect dmesg
    dmesg -c > "$outdir/dmesg"

    # append protocol to results
    echo '' >> $OUTPUT_ROOT/results
    cat "$outdir/protocol.log" >> $OUTPUT_ROOT/results

    # check test result
    test_result="FAIL"
    if [ -e $logfile ]; then
        if ! grep -q FAIL $logfile; then
            test_result="PASS"
        fi
    fi

    # console reporting
    if [ "$test_result" == "FAIL" ]; then
        touch "$outdir/failed"
        echo_failure
    else
        echo_success
    fi
    echo " | $short_testname"

    if [ "$test_result" == "FAIL" ]; then
        for ctest in $TEST_CRITICAL; do
            if [ "$test" == "$ctest/runtest.sh" ]; then
                echo_failure
                echo -n " | Critical test failed"
                if [ "$TEST_CONTINUE" = "0" ]; then
                    echo ", stopping further testing"
                    crit_test_fail=1
                    break
                else
                    echo ", TEST_CONTINUE in effect, resuming testing"
                fi
            fi
        done
        if [ $crit_test_fail -eq 1 ]; then
            break
        fi
    fi

done

if grep -q FAIL $OUTPUT_ROOT/results; then
    RESULT="FAIL"
else
    RESULT="PASS"
fi

if [ $crit_test_fail -eq 1 ]; then
    RESULT="FAIL"
fi

export RESULT
