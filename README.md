# OS Project - Green Scheduler

A carbon-aware task scheduler that delays non-urgent tasks when carbon intensity is high, helping reduce environmental impact.

## Features

- **Carbon-aware scheduling**: Monitors UK carbon intensity API and delays tasks during high carbon periods
- **Priority-based execution**: High urgency tasks run immediately, low urgency tasks are delayed when carbon intensity is high
- **Live dashboard**: Real-time visualization of scheduler performance and carbon intensity trends
- **Mock API**: Optional local carbon intensity API for testing

## Prerequisites (Arch Linux)

The project requires:
- GCC compiler
- libcurl (for HTTP requests)
- json-c (for JSON parsing)
- Python 3 with pandas, matplotlib, flask, numpy

## Quick Setup

### Option 1: Automated Setup (Recommended)

Run the setup script:

```bash
./setup_arch.sh
```

This will:
- Install all required dependencies via pacman
- Install Python packages
- Compile the scheduler

### Option 2: Manual Setup

1. **Install system dependencies:**
   ```bash
   sudo pacman -S --needed base-devel gcc curl json-c python python-pip python-pandas python-matplotlib python-flask
   ```

2. **Install Python dependencies:**
   ```bash
   pip install --user flask pandas matplotlib numpy
   ```

3. **Compile the scheduler:**
   ```bash
   gcc os.c -o green_scheduler -lcurl -ljson-c
   ```

## Running the Project

### 1. Run the Scheduler

**Foreground mode (for debugging):**
```bash
./green_scheduler -f
```

**Background mode (daemon):**
```bash
./green_scheduler
```

The scheduler will:
- Load tasks from `tasks.json`
- Check carbon intensity every 5 minutes
- Execute tasks based on urgency and carbon intensity
- Log all activity to `/tmp/scheduler.log`

### 2. View Logs

```bash
tail -f /tmp/scheduler.log
```

### 3. Run the Live Dashboard (Optional)

In a separate terminal:
```bash
python live_dashboard.py
```

This opens a real-time visualization showing:
- Carbon emission comparisons
- Task delay over time
- Average delay by urgency
- Carbon intensity trends

### 4. Run Mock Carbon API (Optional)

If you want to test with a local mock API instead of the real UK API:

```bash
python mock_carbon_api.py
```

Then update `os.c` line 18 to use:
```c
#define CARBON_API_URL "http://127.0.0.1:5000/intensity"
```

Recompile after changing:
```bash
gcc os.c -o green_scheduler -lcurl -ljson-c
```

## Configuration

### Tasks Configuration

Edit `tasks.json` to add or modify tasks:

```json
[
  {
    "command": "sleep 10",
    "urgency": "low",
    "deadline_hours": 24,
    "submitted_at": 1730822400
  },
  {
    "command": "echo High priority task",
    "urgency": "high",
    "deadline_hours": 1,
    "submitted_at": 1730822400
  }
]
```

**Fields:**
- `command`: Shell command to execute
- `urgency`: `"low"`, `"medium"`, or `"high"`
- `deadline_hours`: Hours until deadline
- `submitted_at`: Unix timestamp of submission

### Scheduler Behavior

- **High urgency tasks**: Always run immediately
- **Low/Medium urgency tasks**: 
  - Delayed if carbon intensity is "high" or "very high" (unless deadline is approaching)
  - Run when intensity is "low" or "moderate"
  - Run if deadline is approaching regardless of intensity

## Project Structure

```
OS_project-main/
├── os.c                    # Main scheduler (C)
├── green_scheduler         # Compiled binary
├── tasks.json              # Task configuration
├── live_dashboard.py       # Real-time dashboard
├── mock_carbon_api.py      # Mock carbon API server
├── setup_arch.sh          # Arch Linux setup script
├── os_rust/               # Rust version (optional)
└── README.md              # This file
```

## Troubleshooting

### Compilation Errors

If you get linker errors:
```bash
# Make sure libraries are installed
sudo pacman -S curl json-c

# Check if libraries are found
pkg-config --libs libcurl json-c
```

### Permission Errors

If you get permission errors for `/var/run/green_scheduler.pid`:
```bash
# Run with sudo (for daemon mode) or use -f flag for foreground
sudo ./green_scheduler
# OR
./green_scheduler -f
```

### Python Import Errors

If Python can't find packages:
```bash
# Install with --user flag
pip install --user flask pandas matplotlib numpy

# Or use system-wide installation
sudo pip install flask pandas matplotlib numpy
```

### Log File Issues

If logs aren't appearing:
```bash
# Check if log file exists and is writable
ls -l /tmp/scheduler.log
touch /tmp/scheduler.log
chmod 666 /tmp/scheduler.log
```

## Notes

- The scheduler checks carbon intensity every 5 minutes
- Tasks are reloaded from `tasks.json` on each cycle
- The config file path is set in `os.c` (line 15) - update if needed
- Logs are written to `/tmp/scheduler.log`
- PID file is stored at `/var/run/green_scheduler.pid` (requires root for daemon mode)

## License

See project license file for details.
