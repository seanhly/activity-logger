# al: the activity logger

al is a simple CLI-based tool for logging your mouse and keyboard activity to a
file.  This can be useful for time and sleep tracking.

## Installation

```{bash}
make
```

# Usage

Run the bash script `al.sh` to start al in a TMUX session.

```{bash}
./al.sh
```

Alternatively, run al directly:

```{bash}
./al | tee -a ~/.history/activity_log.csv
```
