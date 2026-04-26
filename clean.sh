#!/bin/bash
# clean.sh — removes everything demo.sh creates

# Kill any leftover processes
pkill -f "./peer" 2>/dev/null
pkill -f "./tracker" 2>/dev/null

# Remove compiled binaries
rm -f peer tracker

# Remove peer directories
rm -rf peer1 peer2 peer3 peer4 peer5 peer6 peer7 peer8 peer9 peer10 peer11 peer12 peer13

# Remove tracker shared directory
rm -rf torrents

# Remove logs
rm -rf logs

echo "Clean complete"