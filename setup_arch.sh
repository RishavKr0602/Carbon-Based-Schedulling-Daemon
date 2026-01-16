#!/bin/bash

# Setup script for OS Project on Arch Linux

echo "Setting up OS Project on Arch Linux..."

# Install required system dependencies
echo "Installing system dependencies..."
sudo pacman -S --needed base-devel gcc curl json-c python python-pip python-pandas python-matplotlib python-flask

# Install Python dependencies
echo "Installing Python dependencies..."
pip install --user flask pandas matplotlib numpy

# Compile the C scheduler
echo "Compiling the scheduler..."
gcc os.c -o green_scheduler -lcurl -ljson-c

if [ $? -eq 0 ]; then
    echo "✓ Compilation successful!"
    echo ""
    echo "To run the scheduler:"
    echo "  ./green_scheduler"
    echo ""
    echo "To run in foreground mode (for debugging):"
    echo "  ./green_scheduler -f"
    echo ""
    echo "To view logs:"
    echo "  tail -f /tmp/scheduler.log"
    echo ""
    echo "To run the dashboard:"
    echo "  python live_dashboard.py"
    echo ""
    echo "To run the mock carbon API (optional):"
    echo "  python mock_carbon_api.py"
else
    echo "✗ Compilation failed!"
    exit 1
fi
