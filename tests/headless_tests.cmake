# Checks plumnin-headless end to end; run by ctest as
#   cmake -DHEADLESS=path/to/plumnin-headless -DWORK=scratch/dir -P headless_tests.cmake
# Each check runs the program like a user would and looks at what it wrote. Until the
# consoles are emulated they run the test pattern (--test-rom); each console adds its
# own checks here as it arrives.

if(NOT HEADLESS OR NOT WORK)
  message(FATAL_ERROR "HEADLESS and WORK must be set")
endif()
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

function(run expect_code)
  execute_process(COMMAND "${HEADLESS}" ${ARGN} RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT code EQUAL expect_code)
    message(FATAL_ERROR "plumnin-headless ${ARGN}\nexit code ${code}, expected ${expect_code}\n${out}${err}")
  endif()
  set(output "${out}${err}" PARENT_SCOPE)
endfunction()

# The test pattern draws the same picture on every build: a change here means the
# frame or the way it is hashed changed (on purpose or not; if on purpose, update it).
run(0 --test-rom --frames 120 --expect-hash 36e51785 --quiet)
run(0 --test-rom --system gb --frames 120 --expect-hash 8ed7be75 --quiet)
run(3 --test-rom --frames 120 --expect-hash 00000000 --quiet)
run(0 --test-rom --system snes --frames 10 --json "${WORK}/run.json" --quiet)
file(READ "${WORK}/run.json" json)
if(NOT json MATCHES "\"system\": \"SNES\"" OR NOT json MATCHES "\"width\": 256")
  message(FATAL_ERROR "--json did not describe the run:\n${json}")
endif()

# Two runs give the same frames and sound, and a savestate taken after frame 59 and
# loaded back before frame 60 changes nothing.
set(moves --do "10-80 right" --do "30+60 a" --do "100-150 up b")
run(0 --test-rom --frames 180 ${moves} --hash-log "${WORK}/log.txt" --png-every 60 --out-dir "${WORK}" --quiet)
run(0 --test-rom --frames 180 ${moves} --expect-log "${WORK}/log.txt" --quiet)
run(0 --test-rom --frames 180 ${moves} --expect-log "${WORK}/log.txt" --quiet
    --do "59 savestate ${WORK}/s.state" --do "60 loadstate ${WORK}/s.state")
foreach(f 000000 000060 000120)
  if(NOT EXISTS "${WORK}/frame_${f}.png")
    message(FATAL_ERROR "--png-every did not write frame_${f}.png")
  endif()
endforeach()
# --gif and --avi record the run: a GIF89a and an AVI with its picture and sound.
run(0 --test-rom --frames 120 --do "0-119 a" --gif "${WORK}/run.gif" --avi "${WORK}/run.avi" --quiet)
file(READ "${WORK}/run.gif" head LIMIT 6 HEX)
if(NOT head STREQUAL "474946383961")
  message(FATAL_ERROR "--gif did not write a GIF89a: ${head}")
endif()
file(READ "${WORK}/run.avi" head LIMIT 12 HEX)
if(NOT head MATCHES "^52494646........41564920$")
  message(FATAL_ERROR "--avi did not write an AVI: ${head}")
endif()
file(SIZE "${WORK}/run.avi" size)
if(size LESS 100000)
  message(FATAL_ERROR "--avi wrote only ${size} bytes")
endif()

# A run that goes different is caught on its first different frame.
run(3 --test-rom --frames 180 ${moves} --expect-log "${WORK}/log.txt" --do "100 left")
if(NOT output MATCHES "FAILED: frame 100:")
  message(FATAL_ERROR "the divergence was not reported at frame 100:\n${output}")
endif()

# The same savestate, loaded by a new process, carries on where it was saved (with the
# rest of the script, from frame 60 on).
run(0 --test-rom --load-state "${WORK}/s.state" --frames 120 --hash-log "${WORK}/resumed.txt" --quiet
    --do "0-20 right" --do "0-29 a" --do "40-90 up b")
file(STRINGS "${WORK}/log.txt" full REGEX "^119 ")
file(STRINGS "${WORK}/resumed.txt" resumed REGEX "^59 ")
string(REGEX REPLACE "^[0-9]+ " "" full "${full}")
string(REGEX REPLACE "^[0-9]+ " "" resumed "${resumed}")
if(NOT full STREQUAL resumed)
  message(FATAL_ERROR "a savestate loaded with --load-state did not continue the run: '${full}' vs '${resumed}'")
endif()
# A state of another console is refused.
run(1 --test-rom --system gb --load-state "${WORK}/s.state" --frames 10 --quiet)

# Input scripts reach the game: the test pattern keeps its sprite's place in its RAM
# (x low, x high, y), starting in the middle (120, 104).
file(WRITE "${WORK}/input.txt" "# moves\n10-19 right\n20 dump-ram ${WORK}/ram20.bin\n25+5 p2.down\n")
run(0 --test-rom --frames 40 --input "${WORK}/input.txt" --do "30 dump-ram ${WORK}/ram30.bin" --quiet)
foreach(check "20;820068" "30;82006d")
  list(GET check 0 frame)
  list(GET check 1 want)
  file(READ "${WORK}/ram${frame}.bin" got LIMIT 3 HEX)
  if(NOT got STREQUAL want)
    message(FATAL_ERROR "sprite bytes on frame ${frame}: ${got}, expected ${want}")
  endif()
endforeach()

# Bad scripts and options are refused with exit code 2.
run(2 --test-rom --do "10 jump")
run(2 --test-rom --do "ten start")
run(2 --test-rom --frames)
run(2 --test-rom --no-such-option)
run(2 --test-rom --system megadrive)

# Netplay over UDP on this machine: a host and a guest, each with its own player's
# script, see the same frames as one console fed both scripts.
file(WRITE "${WORK}/p1.txt" "10-40 right\n30+20 a\n100-150 up b\n")
file(WRITE "${WORK}/p2.txt" "5-25 p2.left\n60-90 p2.x p2.down\n120-200 p2.down\n")
execute_process(
  COMMAND "${HEADLESS}" --test-rom --frames 200 --netplay-host 47845 --input "${WORK}/p1.txt"
          --hash-log "${WORK}/net_host.txt" --quiet
  COMMAND "${HEADLESS}" --test-rom --frames 200 --netplay-join 127.0.0.1:47845 --input "${WORK}/p2.txt"
          --hash-log "${WORK}/net_guest.txt" --quiet
  RESULTS_VARIABLE codes OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT codes STREQUAL "0;0")
  message(FATAL_ERROR "netplay host and guest ended with ${codes}\n${out}${err}")
endif()
file(READ "${WORK}/p1.txt" p1)
file(READ "${WORK}/p2.txt" p2)
file(WRITE "${WORK}/both.txt" "${p1}${p2}")
run(0 --test-rom --frames 200 --input "${WORK}/both.txt" --expect-log "${WORK}/net_host.txt" --quiet)
run(0 --test-rom --frames 200 --input "${WORK}/both.txt" --expect-log "${WORK}/net_guest.txt" --quiet)
# And the log really depends on pad 2: without its script it goes different.
run(3 --test-rom --frames 200 --input "${WORK}/p1.txt" --expect-log "${WORK}/net_guest.txt" --quiet)
# Only buttons can be scripted in netplay.
run(2 --test-rom --netplay-host 47846 --do "10 reset")
