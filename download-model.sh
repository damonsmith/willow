#!/bin/bash
# Helper script to download whisper models for Willow

set -e

MODEL_DIR="$HOME/.local/share/willow/models"
WHISPER_REPO="https://huggingface.co/ggerganov/whisper.cpp/resolve/main"

echo "==================================================================="
echo "Willow - Whisper Model Downloader"
echo "==================================================================="
echo ""
echo "Available models:"
echo "  1) tiny.en    (~75MB)  - Fastest, good for commands (recommended)"
echo "  2) base.en    (~142MB) - Better accuracy, still fast"
echo "  3) small.en   (~466MB) - High accuracy, moderate speed"
echo "  4) medium.en  (~1.5GB) - Best accuracy, slower"
echo ""

# Check if model already exists
if [ -f "$MODEL_DIR/ggml-tiny.en.bin" ]; then
    echo "Note: tiny.en model already exists at:"
    echo "  $MODEL_DIR/ggml-tiny.en.bin"
    echo ""
fi

read -p "Select model to download [1-4, default: 1]: " choice
choice=${choice:-1}

case $choice in
    1)
        MODEL="tiny.en"
        SIZE="75MB"
        ;;
    2)
        MODEL="base.en"
        SIZE="142MB"
        ;;
    3)
        MODEL="small.en"
        SIZE="466MB"
        ;;
    4)
        MODEL="medium.en"
        SIZE="1.5GB"
        ;;
    *)
        echo "Invalid choice. Defaulting to tiny.en"
        MODEL="tiny.en"
        SIZE="75MB"
        ;;
esac

MODEL_FILE="ggml-$MODEL.bin"
MODEL_PATH="$MODEL_DIR/$MODEL_FILE"

echo ""
echo "Downloading: $MODEL ($SIZE)"
echo "Destination: $MODEL_PATH"
echo ""

# Create directory
mkdir -p "$MODEL_DIR"

# Download with progress
if command -v curl &> /dev/null; then
    curl -L --progress-bar "$WHISPER_REPO/$MODEL_FILE" -o "$MODEL_PATH"
elif command -v wget &> /dev/null; then
    wget --show-progress "$WHISPER_REPO/$MODEL_FILE" -O "$MODEL_PATH"
else
    echo "ERROR: Neither curl nor wget found. Please install one of them."
    exit 1
fi

echo ""
echo "==================================================================="
echo "Download complete!"
echo ""
echo "Model saved to: $MODEL_PATH"
echo ""

# Set up config with the selected model
CONFIG_DIR="$HOME/.config/willow"
CONFIG_FILE="$CONFIG_DIR/config.json"
mkdir -p "$CONFIG_DIR"

if [ -f "$CONFIG_FILE" ]; then
    # Update existing config - replace or add whisper_model key
    if grep -q '"whisper_model"' "$CONFIG_FILE"; then
        sed -i "s/\"whisper_model\":.*$/\"whisper_model\": \"$MODEL_FILE\",/" "$CONFIG_FILE"
    else
        # Insert whisper_model after the opening brace
        sed -i "s/^{$/{\n  \"whisper_model\": \"$MODEL_FILE\",/" "$CONFIG_FILE"
    fi
    echo "Updated config: $CONFIG_FILE"
    echo "  whisper_model: $MODEL_FILE"
else
    # Create a default config with the model set
    cat > "$CONFIG_FILE" << EOF
{
  "hotword": "hey willow",
  "command_threshold": 80,
  "processing_interval": 1.5,
  "gpu_acceleration": false,
  "whisper_model": "$MODEL_FILE",
  "commands": []
}
EOF
    echo "Created config: $CONFIG_FILE"
    echo "  whisper_model: $MODEL_FILE"
fi

echo ""
echo "Restart the service to use the new model:"
echo "  systemctl --user restart willow.service"
echo "==================================================================="
