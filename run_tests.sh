#!/usr/bin/env bash
# run_tests.sh - build fixtures and test ./myls against GNU ls and against stat(1).
# Usage: ./run_tests.sh [path-to-myls]      (exit status = number of failed tests, capped at 100)
export LC_ALL=C TZ=UTC
MYLS=$(readlink -f "${1:-./myls}")
ROOT=${ROOT:-/tmp/myls_lab}
# unprivileged user must be able to execute the binary even if $HOME is not world-traversable
NOBODY_BIN=$(mktemp /tmp/myls_bin.XXXXXX); cp "$MYLS" "$NOBODY_BIN"; chmod 755 "$NOBODY_BIN"
trap 'rm -f "$NOBODY_BIN"' EXIT
pass=0; fail=0

ok()   { pass=$((pass+1)); printf '  PASS  %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  FAIL  %s\n' "$1"; }
title(){ printf '\n== %s ==\n' "$1"; }

# check <args...>: run GNU ls and myls with identical args; stdout, stderr and exit code must match.
check() {
  local label exp got expc gotc
  label="myls $(printf '%q ' "$@")"
  exp=$(command ls "$@" 2>&1); expc=$?
  exp=$(printf '%s' "$exp" | sed 's/^ls:/myls:/')
  got=$("$MYLS" "$@" 2>&1); gotc=$?
  if [[ "$exp" == "$got" && $expc -eq $gotc ]]; then
    ok "$label   (exit $gotc)"
  else
    bad "$label   (exit gnu=$expc mine=$gotc)"
    diff <(printf '%s\n' "$exp") <(printf '%s\n' "$got") | head -15 | sed 's/^/        /'
  fi
}

# same as check but executed as an unprivileged user (root bypasses permission bits)
as_nobody() { setpriv --reuid=65534 --regid=65534 --clear-groups "$@"; }
check_nobody() {
  local label exp got expc gotc
  label="[uid 65534] myls $(printf '%q ' "$@")"
  exp=$(as_nobody ls "$@" 2>&1); expc=$?
  exp=$(printf '%s' "$exp" | sed 's/^ls:/myls:/')
  got=$(as_nobody "$NOBODY_BIN" "$@" 2>&1); gotc=$?
  if [[ "$exp" == "$got" && $expc -eq $gotc ]]; then ok "$label   (exit $gotc)"
  else bad "$label   (exit gnu=$expc mine=$gotc)"
       diff <(printf '%s\n' "$exp") <(printf '%s\n' "$got") | head -15 | sed 's/^/        /'; fi
}

# ------------------------------------------------------------------ fixtures
build_fixtures() {
  chmod -R u+rwx "$ROOT" 2>/dev/null; rm -rf "$ROOT"; mkdir -p "$ROOT"; cd "$ROOT" || exit 99
  chmod 755 "$ROOT"
  mkdir -p empty flat hidden nested/a/b/c nested/e links perms sizes times special big longname dirs
  for f in a.txt B.txt c.log 10.txt 9.txt "with space.txt" "naïve.txt" Zeta alpha; do echo "data $f" > "flat/$f"; done
  echo x > hidden/.secret; mkdir hidden/.config; echo y > hidden/.config/app.conf; echo z > hidden/visible.txt
  echo 1 > nested/a/b/c/d.txt; echo 2 > nested/a/x.txt; echo 3 > nested/top.txt
  echo target > links/real.txt; ln -s real.txt links/soft.txt; ln -s ../flat links/dirlink
  ln -s nowhere links/dangling; ln links/real.txt links/hard.txt; ln -s /etc/passwd links/abs
  for m in 000 400 440 444 600 620 640 644 660 664 666 700 711 740 750 755 770 775 777 \
           4755 4644 2755 2644 6755 6644 1755 1644 7777 7000; do
    touch "perms/f_$m"; chmod "$m" "perms/f_$m"; done
  for m in 700 755 1777 2775 4755 3777 000 555; do mkdir "perms/d_$m"; chmod "$m" "perms/d_$m"; done
  for sz in 0 1 1023 1024 1025 1500 10240 1048576 5000000; do truncate -s "$sz" "sizes/s_$sz"; done
  touch -d '30 days ago'   times/recent_30d.txt
  touch -d '300 days ago'  times/older_300d.txt
  touch -d '2000 days ago' times/ancient.txt
  touch -d '1 day ago'     times/yesterday.txt
  touch -d 'tomorrow'      times/future.txt
  touch -d @1600000000     times/tie_b.txt; touch -d @1600000000 times/tie_a.txt
  mkfifo special/pipe
  mknod special/null c 1 3 2>/dev/null; mknod special/loop0 b 7 0 2>/dev/null
  mknod special/tty0 c 4 0 2>/dev/null;  mknod special/pts12 c 136 12 2>/dev/null; mknod special/big c 250 1024 2>/dev/null
  python3 -c "import socket;s=socket.socket(socket.AF_UNIX);s.bind('special/sock')"
  (cd big && seq -f 'file_%04g' 1 1500 | xargs touch)
  touch "longname/$(printf 'x%.0s' $(seq 1 200)).txt"
  mkdir -p dirs/ok/sub dirs/locked/inner dirs/listonly dirs/searchonly
  echo hi > dirs/ok/sub/f; echo hi > dirs/listonly/a; echo hi > dirs/listonly/b; echo hi > dirs/searchonly/known
  chmod 000 dirs/locked; chmod 444 dirs/listonly; chmod 111 dirs/searchonly
  chmod 644 times/* sizes/* 2>/dev/null
}

echo "myls test-suite   $(date -u '+%F %T UTC')"
echo "binary : $MYLS"
echo "gnu ls : $(ls --version | head -1)"
echo "kernel : $(uname -sr)    running as uid $(id -u)"
build_fixtures
echo "fixtures built in $ROOT: $(find "$ROOT" | wc -l) filesystem objects"
# guard: if a fixture is missing, 'both programs fail identically' would be a false PASS
missing=0
for p in empty flat/a.txt hidden/.secret hidden/.config/app.conf nested/a/b/c/d.txt nested/e links/dirlink links/dangling \
         perms/f_7777 sizes/s_5000000 times/future.txt special/pipe special/sock big/file_1500 dirs/locked dirs/listonly; do
  [[ -e $p || -L $p ]] || { echo "  FIXTURE MISSING: $p"; missing=1; }
done
[[ $missing -eq 0 ]] && echo "fixture sanity check: all expected paths exist" || exit 99

# ------------------------------------------------------------------ 1. basic listings
title "1. Basic listing (compared byte-for-byte with GNU ls)"
check flat
check -1 flat
check empty
check -a empty
check hidden
check -a hidden
check -r flat
check -ra flat
check -l flat
check -la flat
check -i flat
check -il flat
check -d flat
check -ld flat hidden perms/d_1777
check -ld .
check -d links/dirlink
check -ld links/dirlink

# ------------------------------------------------------------------ 2. structures
title "2. Directory structures"
check nested
check -R nested
check -R .
check -laR nested
check -R hidden
check -aR hidden
check flat hidden
check flat empty hidden
check flat/a.txt flat/B.txt
check flat/a.txt hidden
check -l flat/a.txt hidden empty
check big
check -l big
check -R big
check longname
check -l longname
check . ..
check ./flat/
check flat//

# ------------------------------------------------------------------ 3. sorting & formats
title "3. Sorting and formatting options"
check -t times
check -tr times
check -lt times
check -S sizes
check -Sr sizes
check -lS sizes
check -lh sizes
check -lhS sizes
check -F flat
check -F links
check -lF links
check -F special
check -l times
check -l -t -r times

# ------------------------------------------------------------------ 4. links and specials
title "4. Symbolic links, hard links, special files"
check -l links
check links
check links/dirlink
check -l links/dirlink
check -l links/dangling links/soft.txt
check -i links
check -l special
check special

# ------------------------------------------------------------------ 5. permission flags
title "5. Permission flags (ls -l mode string vs stat(1), octal via -O)"
perm_dir() {  # compare myls -lO against stat for every entry of a directory
  local d=$1 exp got
  exp=$(cd "$d" && for f in $(command ls -A); do
          printf '%04d %s %s\n' "$(stat -c %a "$f")" "$(stat -c %A "$f")" "$f"; done)
  got=$("$MYLS" -laO "$d" | awk 'NR>1 { n=$NF; for(i=1;i<=NF;i++) if($i=="->"){n=$(i-1)}; if(n!="." && n!="..") print $1, $2, n }')
  if [[ "$exp" == "$got" ]]; then ok "perm bits of $d vs stat: $(printf '%s\n' "$exp" | wc -l) entries identical"
  else bad "perm bits of $d vs stat"; diff <(echo "$exp") <(echo "$got") | head -10 | sed 's/^/        /'; fi
}
perm_dir perms
perm_dir sizes
perm_dir links
perm_dir special

echo "  --- sample of the permission fixtures as listed by myls -lO ---"
"$MYLS" -lO perms | sed 's/^/        /'

# exhaustive: every one of the 4096 permission combinations (rwx x3 + suid/sgid/sticky)
mkdir -p allperms
python3 - <<'PY'
import os
for i in range(4096):
    p = f"allperms/m{i:04o}"
    open(p, "w").close(); os.chmod(p, i)
PY
python3 - "$MYLS" <<'PY'
import os, stat, subprocess, sys
myls = sys.argv[1]
out = subprocess.run([myls, "-lO", "allperms"], capture_output=True, text=True).stdout.splitlines()[1:]
bad = 0
for line in out:
    octal, sym, *_rest, name = line.split()
    mode = os.lstat(f"allperms/{name}").st_mode
    want_sym = stat.filemode(mode)                      # Python's independent implementation
    want_oct = f"{mode & 0o7777:04o}"
    if sym != want_sym or octal != want_oct or name != f"m{want_oct}":
        bad += 1
        if bad <= 5: print("        MISMATCH", line, want_sym, want_oct)
print(f"  {'PASS' if bad == 0 and len(out) == 4096 else 'FAIL'}  exhaustive: {len(out)} files, all 4096 mode patterns "
      f"(0000-7777) match Python stat.filemode(): mismatches = {bad}")
sys.exit(1 if bad or len(out) != 4096 else 0)
PY
[[ $? -eq 0 ]] && pass=$((pass+1)) || fail=$((fail+1))

# file-type characters: - d l c b p s
types=$("$MYLS" -l special links flat/a.txt nested 2>/dev/null | awk 'NF>=9 && $1 ~ /^[-dlcbps]/ {print substr($1,1,1)}' | sort -u | tr -d '\n')
[[ "$types" == *-* && "$types" == *d* && "$types" == *l* && "$types" == *p* && "$types" == *s* ]] \
  && ok "file-type characters seen in output: '$types' (regular, dir, symlink, fifo, socket$( [[ $types == *c* ]] && echo ', char-dev'))" \
  || bad "file-type characters seen: '$types'"

# ------------------------------------------------------------------ 6. errors & access control
title "6. Error handling and permission enforcement"
check nosuchdir
check flat nosuchdir hidden
check -l nosuchfile flat/a.txt
echo "  --- running as unprivileged uid 65534 (root would bypass permission bits) ---"
check_nobody dirs/locked
check_nobody -l dirs/locked
check_nobody dirs/listonly
check_nobody dirs/searchonly
check_nobody -R dirs
check_nobody -l perms/f_000 perms/f_4755
check_nobody -lh sizes
echo "  --- -l inside a list-only (r--) directory: entries cannot be stat()ed ---"
got=$(as_nobody "$NOBODY_BIN" -l dirs/listonly 2>&1 >/dev/null); gotc=$?
exp=$(as_nobody ls -l dirs/listonly 2>&1 >/dev/null | sed 's/^ls:/myls:/')
if [[ "$got" == "$exp" && $gotc -eq 1 ]]; then ok "stderr identical to GNU ls, exit status 1 (minor problem): $(echo "$got" | head -1)"
else bad "list-only -l stderr/exit: exit=$gotc"; fi
check_exit() { "$MYLS" "$@" >/dev/null 2>&1; echo $?; }
[[ $(check_exit -Z flat) -eq 2 ]] && ok "unknown option -Z -> usage message, exit 2" || bad "unknown option exit status"

# ------------------------------------------------------------------ 7. robustness
title "7. Robustness"
n=$("$MYLS" -1 big | wc -l); [[ $n -eq 1500 ]] && ok "1500-entry directory fully read through repeated getdents64 calls ($n lines)" || bad "big dir count $n"
n=$("$MYLS" -1a big | wc -l); [[ $n -eq 1502 ]] && ok "-a adds . and .. ($n lines)" || bad "-a count $n"
out=$("$MYLS" -1 flat | tr '\n' '|'); [[ "$out" == '10.txt|9.txt|B.txt|Zeta|a.txt|alpha|c.log|naïve.txt|with space.txt|' ]] \
  && ok "byte-order (C locale) sort: $out" || bad "sort order: $out"
check -l /dev/null
check /proc/self/fd

# ------------------------------------------------------------------ 8. real system trees
title "8. Real-world trees (thousands of entries, compared with GNU ls)"
for d in /usr/include /etc /usr/bin /dev; do
  [[ -d $d ]] || { echo "  SKIP  $d not present"; continue; }
  case $d in
    /usr/include) check -R "$d"; check -laR "$d" ;;
    /etc)         check -lR "$d"; check -laiR "$d" ;;
    /usr/bin)     check -lS "$d"; check -FR "$d"; check -lhtr "$d" ;;
    /dev)         check -l "$d"; check -la "$d"; check -lh "$d" ;;
  esac
done

echo
echo "================ SUMMARY: $pass passed, $fail failed ================"
chmod -R u+rwx "$ROOT" 2>/dev/null
exit $(( fail > 100 ? 100 : fail ))
