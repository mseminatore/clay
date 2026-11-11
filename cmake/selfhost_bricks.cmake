# selfhost_bricks.cmake — `clayc bricks` (selfhost/clay-bricks, a port of
# src/compiler/bricks.c) must do what `clay bricks` does: the same output,
# exit status, brick.json, brick.lock and bricks/ directory, command by
# command, in two copies of a project. clayc runs on the VM here
# (`clay selfhost/clayc.clay bricks ...`); cmake/selfhost_run.cmake checks a
# few commands with the native clayc.
#
# Nothing touches the network: the dependencies are git repositories made
# here, which a git config (GIT_CONFIG_GLOBAL) puts in place of
# https://example.test/. Skipped without git 2.32 or later.
#
#   cmake -DCLAY=<clay binary> -DROOT=<source dir> -DWORK=<scratch dir>
#         -P selfhost_bricks.cmake

foreach(var CLAY ROOT WORK)
    if(NOT ${var})
        message(FATAL_ERROR "selfhost_bricks.cmake: ${var} is not set")
    endif()
endforeach()

find_program(GIT git)
if(NOT GIT)
    message(STATUS "skipped: git not found")
    return()
endif()
execute_process(COMMAND "${GIT}" --version OUTPUT_VARIABLE git_version)
string(REGEX MATCH "[0-9]+\\.[0-9]+" git_version "${git_version}")
if(git_version VERSION_LESS 2.32)
    message(STATUS "skipped: git ${git_version} is older than 2.32 (GIT_CONFIG_GLOBAL)")
    return()
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/clay/proj" "${WORK}/clayc/proj" "${WORK}/src" "${WORK}/repos/acme")
file(WRITE "${WORK}/empty" "")

# --- git: only this configuration, with the repositories below standing in
# for https://example.test/
if(WORK MATCHES "^/")
    set(repos_url "file://${WORK}/repos/")
else()
    set(repos_url "file:///${WORK}/repos/")     # a Windows drive path
endif()
file(WRITE "${WORK}/gitconfig"
"[url \"${repos_url}\"]
	insteadOf = https://example.test/
[protocol \"file\"]
	allow = always
[user]
	name = clay
	email = clay@example.test
[init]
	defaultBranch = main
[commit]
	gpgsign = false
[core]
	autocrlf = false
[advice]
	detachedHead = false
")
set(ENV{GIT_CONFIG_GLOBAL} "${WORK}/gitconfig")
set(ENV{GIT_CONFIG_NOSYSTEM} 1)

function(git dir)
    execute_process(COMMAND "${GIT}" ${ARGN} WORKING_DIRECTORY "${dir}"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "git ${ARGN} failed in ${dir} (${rc}):\n${out}")
    endif()
endfunction()

# commit everything in src/<name> with message <msg>
function(commit name msg)
    if(NOT EXISTS "${WORK}/src/${name}/.git")
        git("${WORK}/src/${name}" init -q)
    endif()
    git("${WORK}/src/${name}" add -A)
    git("${WORK}/src/${name}" commit -q -m "${msg}")
endfunction()

function(publish name)
    git("${WORK}" clone -q --bare "src/${name}" "repos/acme/${name}")
endfunction()

# lib-b: no dependencies; tagged v1.0, then one more commit
file(WRITE "${WORK}/src/lib-b/brick.json"
     "{\n  \"name\": \"lib-b\",\n  \"version\": \"1.0.0\",\n  \"main\": \"src/main.clay\",\n  \"deps\": {}\n}\n")
file(WRITE "${WORK}/src/lib-b/src/main.clay" "LibB = Object clone { v() { 1 } }\n")
commit(lib-b one)
git("${WORK}/src/lib-b" tag v1.0)
file(WRITE "${WORK}/src/lib-b/src/main.clay" "LibB = Object clone { v() { 2 } }\n")
commit(lib-b two)
# lib-a: depends on lib-b (and has a key Clay doesn't know)
file(WRITE "${WORK}/src/lib-a/brick.json"
     "{\n  \"name\": \"lib-a\",\n  \"version\": \"0.1.0\",\n  \"license\": \"MIT\",\n  \"deps\": {\n    \"lib-b\": \"example.test/acme/lib-b\"\n  }\n}\n")
file(WRITE "${WORK}/src/lib-a/src/main.clay" "LibA = Object clone\n")
commit(lib-a one)
# mono: two bricks in one repository, one depending on the other
file(WRITE "${WORK}/src/mono/json/brick.json" "{ \"name\": \"json\", \"version\": \"0.1.0\" }\n")
file(WRITE "${WORK}/src/mono/text/brick.json"
     "{ \"name\": \"text\", \"deps\": { \"mono/json\": \"example.test/acme/mono/json\" } }\n")
commit(mono one)
# nat: a native brick
file(WRITE "${WORK}/src/nat/brick.json"
     "{ \"name\": \"nat\", \"version\": \"0.1.0\",
  \"native\": { \"plugin\": \"nat\", \"sources\": [\"native/a.c\", \"native/b.c\"], \"cflags\": [\"-O2\"],
              \"libs-linux\": [\"-lm\"], \"libs-macos\": [\"-lm\"], \"libs-windows\": [] } }\n")
file(WRITE "${WORK}/src/nat/native/a.c" "/* a */\n")
file(WRITE "${WORK}/src/nat/native/b.c" "/* b */\n")
commit(nat one)
# cyc-a and cyc-b depend on each other
file(WRITE "${WORK}/src/cyc-a/brick.json"
     "{ \"name\": \"cyc-a\", \"deps\": { \"cyc-b\": \"example.test/acme/cyc-b\" } }\n")
file(WRITE "${WORK}/src/cyc-b/brick.json"
     "{ \"name\": \"cyc-b\", \"deps\": { \"cyc-a\": \"example.test/acme/cyc-a\" } }\n")
commit(cyc-a one)
commit(cyc-b one)
# bad: a dependency that must be rejected
file(WRITE "${WORK}/src/bad/brick.json"
     "{ \"name\": \"bad\", \"deps\": { \"x\": \"example.test/acme/x y\" } }\n")
commit(bad one)
foreach(name lib-a lib-b mono nat cyc-a cyc-b bad)
    publish(${name})
endforeach()

# --- the two projects
set(failed "")
set(steps 0)
set(last "")

# Run `bricks <args>` with clay and with clayc, each in its project; input is
# an empty file, so nothing waits for an answer
macro(both)
    execute_process(COMMAND "${CLAY}" bricks ${ARGN}
                    WORKING_DIRECTORY "${WORK}/clay/proj" INPUT_FILE "${WORK}/empty" TIMEOUT 300
                    RESULT_VARIABLE rc_a OUTPUT_VARIABLE out_a ERROR_VARIABLE out_a)
    execute_process(COMMAND "${CLAY}" "${ROOT}/selfhost/clayc.clay" bricks ${ARGN}
                    WORKING_DIRECTORY "${WORK}/clayc/proj" INPUT_FILE "${WORK}/empty" TIMEOUT 300
                    RESULT_VARIABLE rc_b OUTPUT_VARIABLE out_b ERROR_VARIABLE out_b)
    string(REPLACE "\r" "" out_a "${out_a}")
    string(REPLACE "\r" "" out_b "${out_b}")
    string(REPLACE "clayc bricks" "clay bricks" out_b "${out_b}")
    math(EXPR steps "${steps} + 1")
    set(last "${out_a}")
    if(NOT rc_a STREQUAL rc_b OR NOT out_a STREQUAL out_b)
        list(APPEND failed "bricks ${ARGN}")
        message("--- bricks ${ARGN}: clay (exit ${rc_a}) ---\n${out_a}--- clayc (exit ${rc_b}) ---\n${out_b}")
    endif()
endmacro()

# what the last `clay bricks` printed (so the comparison isn't of two failures)
macro(expect text)
    string(FIND "${last}" "${text}" at)
    if(at LESS 0)
        list(APPEND failed "expected \"${text}\"")
        message("--- expected \"${text}\" in:\n${last}")
    endif()
endmacro()

function(read_state dir out)
    set(state "")
    foreach(f brick.json brick.lock)
        if(EXISTS "${dir}/${f}")
            file(READ "${dir}/${f}" text)
            string(REPLACE "\r" "" text "${text}")
        else()
            set(text "(none)\n")
        endif()
        string(APPEND state "--- ${f}\n${text}")
    endforeach()
    file(GLOB entries RELATIVE "${dir}" "${dir}/bricks/*" "${dir}/bricks/*/*")
    list(SORT entries)
    string(REPLACE ";" "\n" entries "${entries}")
    set(${out} "${state}--- bricks/\n${entries}\n" PARENT_SCOPE)
endfunction()

macro(same_state what)
    read_state("${WORK}/clay/proj" state_a)
    read_state("${WORK}/clayc/proj" state_b)
    if(NOT state_a STREQUAL state_b)
        list(APPEND failed "files ${what}")
        message("--- files ${what}: clay ---\n${state_a}--- clayc ---\n${state_b}")
    endif()
endmacro()

macro(write_both file text)
    file(WRITE "${WORK}/clay/proj/${file}" "${text}")
    file(WRITE "${WORK}/clayc/proj/${file}" "${text}")
endmacro()

macro(remove_both)
    foreach(entry ${ARGN})
        file(REMOVE_RECURSE "${WORK}/clay/proj/${entry}" "${WORK}/clayc/proj/${entry}")
    endforeach()
endmacro()

# usage and errors, before there is a brick.json
both()
both(bogus)
both(list)
both(install)
both(update)
both(add example.test/acme/lib-a)
both(remove)
both(remove x)
both(info)
both(add)
both(add a b c)

both(init)
expect("Created brick.json for 'proj'")
both(init)
both(list)
both(install)
both(update)
same_state("after init")

# add: a brick with a dependency, a pinned one under another name, bricks
# inside a repository, a native brick, and what must be refused
both(add example.test/acme/lib-a)
expect("Installing lib-b from https://example.test/acme/lib-b...")
expect("Added 'lib-a' to brick.json")
both(add example.test/acme/lib-a)
both(add "example.test/acme/x y")
both(add example.test/acme)
both(add example.test/acme/lib-b@v1.0 "bad name")
both(add example.test/acme/lib-b@v1.0 libb)
both(add example.test/acme/mono/json)
expect("Installed mono/json")
both(add example.test/acme/mono/text mono2)
both(add -y example.test/acme/nat)
expect("(2 source files)")
both(add example.test/acme/nat nat2)
both(add example.test/acme/missing)
both(add example.test/acme/mono/nope)
both(add example.test/acme/cyc-a)
expect("Added 'cyc-a' to brick.json")
both(add example.test/acme/bad)
expect("removed the partial install bricks/bad")
both(add example.test/acme/lib-b lib-a)
both(add example.test/acme/nat lib-b)
expect("is already installed from")
same_state("after add")

# install: nothing to do, from brick.lock, and without it
both(list)
both(install)
expect("Dependencies already locked and up to date.")
remove_both(bricks/lib-a bricks/libb)
both(install)
expect("(from brick.lock)")
same_state("after install from brick.lock")
remove_both(brick.lock)
both(install)
same_state("after install")

# update: nothing new, then a new commit in lib-a
both(update)
expect("Everything was already up to date.")
file(WRITE "${WORK}/src/lib-a/src/main.clay" "LibA = Object clone { more = 1 }\n")
commit(lib-a two)
git("${WORK}/src/lib-a" push -q "${WORK}/repos/acme/lib-a" HEAD:main)
both(update)
expect("Updated 1 of 7 dependencies.")
remove_both(bricks/libb)
both(update)
expect("libb: not installed, installing...")
same_state("after update")

both(remove lib-a)
both(remove mono/json)
both(remove nope)
both(remove mono2/text)
same_state("after remove")

# add and remove change only "deps": other fields, the native section with its
# per-platform lists, the key order and the formatting stay as written
macro(expect_in_file text)
    file(READ "${WORK}/clay/proj/brick.json" json_now)
    string(FIND "${json_now}" "${text}" at)
    if(at LESS 0)
        list(APPEND failed "brick.json lost: ${text}")
        message("--- brick.json lost \"${text}\":\n${json_now}")
    endif()
endmacro()

write_both(brick.json "{
  \"name\": \"p\",
  \"license\": \"MIT\",
  \"keywords\": [\"a\", \"b\"],
  \"deps\": {
    \"lib-b\": \"example.test/acme/lib-b\"
  },
  \"native\": { \"plugin\": \"p\", \"sources\": [\"a.c\"], \"libs-linux\": [\"-lm\"], \"libs-macos\": [\"-lm\"], \"libs-windows\": [] },
  \"x\": 1
}
")
remove_both(bricks brick.lock)
both(add example.test/acme/lib-a)
expect("Added 'lib-a' to brick.json")
expect_in_file("\"license\": \"MIT\",")
expect_in_file("\"libs-linux\": [\"-lm\"], \"libs-macos\": [\"-lm\"], \"libs-windows\": []")
expect_in_file("    \"lib-a\": \"example.test/acme/lib-a\"")
both(remove lib-b)
expect_in_file("\"keywords\": [\"a\", \"b\"],")
expect_in_file("\"x\": 1")
same_state("after add and remove in a manifest with other fields")
# no "deps" at all, a one-line manifest, and one with Windows line ends
write_both(brick.json "{ \"name\": \"p\", \"license\": \"MIT\" }\n")
remove_both(bricks brick.lock)
both(add example.test/acme/lib-b)
expect_in_file("\"license\": \"MIT\",")
expect_in_file("\"deps\": {")
both(remove lib-b)
expect_in_file("\"deps\": {}")
same_state("after add and remove in a manifest without deps")
write_both(brick.json "{\r\n  \"name\": \"p\",\r\n  \"license\": \"MIT\"\r\n}\r\n")
remove_both(bricks brick.lock)
both(add example.test/acme/lib-b)
same_state("after add in a manifest with Windows line ends")
# (read_state drops \r, so look at the files themselves: same bytes, and
# every line end still \r\n)
file(READ "${WORK}/clay/proj/brick.json" crlf_a HEX)
file(READ "${WORK}/clayc/proj/brick.json" crlf_b HEX)
string(REGEX REPLACE "(..)" "\\1;" crlf_bytes "${crlf_a}")
set(crlf_ok TRUE)
if(NOT crlf_a STREQUAL crlf_b)
    set(crlf_ok FALSE)
endif()
set(prev "")
foreach(byte ${crlf_bytes})
    if(byte STREQUAL "0a" AND NOT prev STREQUAL "0d")
        set(crlf_ok FALSE)          # a bare \n
    endif()
    set(prev "${byte}")
endforeach()
if(NOT crlf_a MATCHES "6c69622d62")      # "lib-b": the new dependency is in
    set(crlf_ok FALSE)
endif()
if(NOT crlf_ok)
    list(APPEND failed "Windows line ends in brick.json")
    message("--- Windows line ends (hex): clay ---\n${crlf_a}\n--- clayc ---\n${crlf_b}")
endif()

# a brick.json as loosely written as the reader allows, then rewritten
write_both(brick.json "{ \"name\": \"p\", \"version\": \"2\", \"deps\": { \"lib-b\": \"example.test/acme/lib-b\" \"cyc-a\": \"example.test/acme/cyc-a\", },
  \"native\": { \"plugin\": \"p\", \"sources\": [\"a.c\"], \"cflags\": [\"-O2\", \"-I@/inc\"], \"libs\": [] },
  \"extra\": [1, {\"a\": \"]\"}], \"n\": 12 }\n")
both(list)
both(install)
both(update)
both(add example.test/acme/lib-a)
both(remove cyc-a)
same_state("after a loosely written brick.json")

# invalid manifests
write_both(brick.json "{ \"name\": \"p\", \"deps\": { \"x\": \"bad url\" } }\n")
both(list)
both(install)
write_both(brick.json "{ \"name\": \"p\", \"native\": { \"plugin\": \"p\", \"sources\": [\"a.c\"], \"cflags\": [\"-O2 -x\"] } }\n")
both(list)
write_both(brick.json "{ \"name\": \"p\", \"native\": { \"plugin\": \"p\" } }\n")
both(update)
write_both(brick.json "[]\n")
both(list)
both(remove x)

if(failed)
    list(LENGTH failed nfail)
    string(REPLACE ";" "\n  " list "${failed}")
    message(FATAL_ERROR "${nfail} of ${steps} bricks commands differ between clay and clayc:\n  ${list}")
endif()
message(STATUS "${steps} bricks commands do the same under clay and clayc")
