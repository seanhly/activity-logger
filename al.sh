#!/usr/bin/bash
if [ "$TMUX" ]; then
	mkdir -p ~/.history
	./al | tee -a ~/.history/activity_log.csv
fi
tmux new-session -d -s activity-logger $0
tmux set-option -t activity-logger remain-on-exit on
