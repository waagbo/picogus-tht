#!/usr/bin/env bash
# Setup script for Claude Code cloud sessions doing KiCad PCB work
# (PicoGUS boards: ERC/DRC, gerbers, renders, pcbnew Python, autorouting).
#
# Installs KiCad 10 (kicad-cli and the pcbnew Python module) with the symbol,
# footprint and 3D-model libraries and KiCad's default library tables,
# Freerouting, gerbv, ngspice, numpy/scipy/shapely/matplotlib/PIL,
# rsvg-convert and poppler. Adds two commands:
#   kipython      Python that can `import pcbnew` (same arguments as python3)
#   freerouting   headless Freerouting, e.g. freerouting -de b.dsn -do b.ses -mp 40 -mt 2
#
# This file is the reference copy of the setup script in the Claude Code cloud
# environment settings (cloud environment menu > Edit > Setup script). Edit it
# here, then paste the new version there; new sessions run the pasted copy.
#
# Runs as root at session start, one directory above the repository. The
# KiCad part is required (the script exits non-zero if it fails); the extras
# only print a warning.

set -uo pipefail
export DEBIAN_FRONTEND=noninteractive

# ---- settings (edit here: the script cannot read the environment's variables) ----
KICAD_SERIES=10.0
INSTALL_3D_MODELS=1        # 1 = kicad-packages3d (~3.2 GB on disk), needed for 3D renders
FREEROUTING_VERSION=2.1.0
KICAD_PPA_FPR=FDA854F61C4D0D9572BB95E5245D5502FAD7A805   # "Launchpad PPA for KiCad"

log()   { printf '\n== %s\n' "$*"; }
warn()  { printf 'WARNING: %s\n' "$*" >&2; }
die()   { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
retry() { local n; for n in 1 2 3 4; do "$@" && return 0; sleep $((n * n * 2)); done; return 1; }

. /etc/os-release

log "KiCad ${KICAD_SERIES} PPA (${VERSION_CODENAME})"
# Written by hand: add-apt-repository breaks when /usr/bin/python3 is not the
# distro Python (it needs apt_pkg).
key=/etc/apt/keyrings/kicad.gpg
install -d -m 0755 /etc/apt/keyrings
key_ok() { gpg --show-keys --with-colons "$key" 2>/dev/null | grep -q "^fpr:::::::::${KICAD_PPA_FPR}:"; }
if ! key_ok; then
    retry sh -c "curl -fsSL 'https://keyserver.ubuntu.com/pks/lookup?op=get&search=0x${KICAD_PPA_FPR}' \
                 | gpg --dearmor --yes -o '$key'" || die "could not fetch the KiCad PPA key"
    key_ok || die "KiCad PPA key fingerprint mismatch"
fi
rm -f /etc/apt/sources.list.d/kicad-ubuntu-kicad-*.sources /etc/apt/sources.list.d/kicad-ubuntu-kicad-*.list
cat > /etc/apt/sources.list.d/kicad.sources <<EOF
Types: deb
URIs: https://ppa.launchpadcontent.net/kicad/kicad-${KICAD_SERIES}-releases/ubuntu/
Suites: ${VERSION_CODENAME}
Components: main
Signed-By: ${key}
EOF

log "apt packages"
retry apt-get update -q || die "apt-get update failed"
core=(kicad kicad-symbols kicad-footprints)
[ "$INSTALL_3D_MODELS" = 1 ] && core+=(kicad-packages3d)
retry apt-get install -y -q --no-install-recommends "${core[@]}" || die "KiCad install failed"
extras=(python3-numpy python3-scipy python3-shapely python3-matplotlib python3-pil
        librsvg2-bin poppler-utils gerbv ngspice zip unzip curl ca-certificates)
command -v java >/dev/null || extras+=(default-jre-headless)
retry apt-get install -y -q --no-install-recommends "${extras[@]}" || warn "some extra packages did not install"

log "KiCad default library tables"
# Without them kicad-cli DRC reports every library footprint as lib_footprint_issues.
cfg="$HOME/.config/kicad/${KICAD_SERIES}"
install -d "$cfg"
for t in /usr/share/kicad/template/*-lib-table; do
    [ -e "$t" ] || continue
    [ -s "$cfg/$(basename "$t")" ] || cp "$t" "$cfg/"
done

log "kipython (Python that imports pcbnew)"
py=""
for c in /usr/bin/python3.12 /usr/bin/python3.13 /usr/bin/python3.11 /usr/bin/python3; do
    [ -x "$c" ] || continue
    if PYTHONPATH=/usr/lib/python3/dist-packages "$c" -c "import pcbnew" >/dev/null 2>&1; then py=$c; break; fi
done
[ -n "$py" ] || die "no Python here can import pcbnew"
cat > /usr/local/bin/kipython <<EOF
#!/bin/sh
# Python that can import pcbnew (KiCad ${KICAD_SERIES}); takes the same arguments as python3.
PYTHONPATH="/usr/lib/python3/dist-packages\${PYTHONPATH:+:\$PYTHONPATH}" exec $py "\$@"
EOF
chmod 0755 /usr/local/bin/kipython

log "Freerouting ${FREEROUTING_VERSION}"
fr=/opt/freerouting/freerouting-${FREEROUTING_VERSION}.jar
install -d /opt/freerouting
if ! unzip -tq "$fr" >/dev/null 2>&1; then
    if retry curl -fsSL -o "$fr.part" \
        "https://github.com/freerouting/freerouting/releases/download/v${FREEROUTING_VERSION}/freerouting-${FREEROUTING_VERSION}.jar"; then
        mv "$fr.part" "$fr"
    else
        rm -f "$fr.part"; warn "Freerouting download failed"
    fi
fi
cat > /usr/local/bin/freerouting <<EOF
#!/bin/sh
# Headless Freerouting ${FREEROUTING_VERSION}: freerouting -de board.dsn -do board.ses -mp 40 -mt 2
exec java -jar $fr --gui.enabled=false "\$@"
EOF
chmod 0755 /usr/local/bin/freerouting

log "check"
kicad-cli version || die "kicad-cli is missing"
kipython -c "import pcbnew; print('pcbnew', pcbnew.Version())" 2>/dev/null | tail -1
kipython -c "import numpy, scipy, shapely, matplotlib; print('numpy', numpy.__version__, 'scipy', scipy.__version__, 'shapely', shapely.__version__)" \
    || warn "numeric Python is incomplete"
[ -s "$fr" ] && echo "freerouting: $fr" || warn "Freerouting is missing"
for c in java gerbv ngspice rsvg-convert pdftoppm; do command -v "$c" >/dev/null && echo "found: $c" || warn "$c is missing"; done
[ -d /usr/share/kicad/3dmodels ] && echo "3D models: $(du -sh /usr/share/kicad/3dmodels | cut -f1)"
df -h / | tail -1
exit 0
