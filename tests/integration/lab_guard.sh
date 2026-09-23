#!/usr/bin/env bash
# lab_guard.sh -- refuse to operate on a LAB path that is not clearly ours.
#
# setup_lab.sh runs   rm -rf "$LAB/run" "$LAB/logs" ...   and
# teardown_lab.sh runs rm -rf "$LAB"
# both as root, and $LAB comes straight from the IPC_LAB environment variable.
# A typo (IPC_LAB=/ , IPC_LAB=/usr , IPC_LAB=$HOME) therefore turns a test
# helper into a system wipe.  common.sh's comment calls the lab "a
# self-contained, clearly-named tree that cannot collide with a production
# socket root" -- this file is what makes that a property of the code rather
# than a property of the comment.
#
# Sourced by common.sh, run_all.sh, setup_lab.sh and teardown_lab.sh so every
# entry point applies the same rule.
#
# The rule is deliberately narrow: an absolute path, at least two components
# deep, whose last component contains "lab".  Every system root (/ , /usr ,
# /etc , /home , /root , /var , /tmp , /opt) is either one component or does not
# end in "lab", so none of them can pass, and neither can $HOME.
#
# This is a guard against accidents, not against a hostile caller: anyone who
# can set IPC_LAB can also edit this file.

# guard_lab_path <path> -- 0 when the path is safe to create/delete wholesale.
guard_lab_path() {
    local lab="$1" last

    if [ -z "$lab" ]; then
        echo "REFUSING: LAB is empty" >&2
        return 1
    fi
    case "$lab" in
        /*) ;;
        *)
            echo "REFUSING: LAB=$lab is not an absolute path" >&2
            return 1
            ;;
    esac
    # At least two components: /opt/ipc-lab is fine, /opt and / are not.
    case "${lab#/}" in
        */*) ;;
        *)
            echo "REFUSING: LAB=$lab is too shallow (want something like /opt/ipc-lab)" >&2
            return 1
            ;;
    esac
    # The leaf has to say what it is, so an accidental /opt/foo cannot be
    # deleted wholesale.
    last="${lab##*/}"
    case "$last" in
        *lab*) ;;
        *)
            echo "REFUSING: LAB=$lab does not look like a test lab (the last " \
                 "component must contain 'lab', e.g. ipc-lab)" >&2
            return 1
            ;;
    esac
    if [ -n "${HOME:-}" ] && [ "$lab" = "$HOME" ]; then
        echo "REFUSING: LAB=$lab is \$HOME" >&2
        return 1
    fi
    return 0
}
