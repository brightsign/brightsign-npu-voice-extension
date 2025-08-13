# BrightSign Voice Detection Extension

**Automated Voice detection extension for BrightSign Series 5 players using Rockchip NPU acceleration.**

This project provides a complete automated build system for BrightSign extensions that combine:

**RetinaFace-based gaze detection** - detects faces and determines if people are looking at the screen.

**Whisper encoder-decoder voice recognition** - transcribes speech to text in real-time when user speaks.

**NPU acceleration** - runs both AI models simultaneously on NPU.

## 🚀 Quick Start (Complete Automated Workflow)

__Total Time__: 60-90 minutes | __Prerequisites__: Docker, git, x86_64 Linux host

> ⏱️ **Time Breakdown**: Most time is spent in the OpenEmbedded SDK build (30-45 min). The process is fully automated but requires patience for the BitBake compilation.

```bash
# Clone the repository (1-2 minutes)
git clone <repository-url>
cd brightsign-npu-voice-extension

# This script automates the entire build process for the BrightSign NPU Voice Extension. This script
# will do the following if the first time otherwise it will skip the steps that have already been done:

# 1. Setup and compile ONNX models to RKNN format (3-5 minutes)
# 2. Build OpenEmbedded SDK (30-45 minutes)
# 3. Install the SDK (1 minute)
# 4. Build C++ applications for all platforms (3-8 minutes)
# 5. Package extension for deployment (1 minute)
./scripts/runall.sh

```

In a typical development workflow, steps 1-3 (setup, model compilation, build and install the SDK) will need to only be done once. Building the apps and packaging them will likely be repeated as the developer changes the app code.

**✅ Success**: You now have production-ready extension packages:

- `voice-dev-<timestamp>.zip` (development/testing)
- `voice-ext-<timestamp>.zip` (production deployment)

**Unsecure the Player**

* This needs to be done if you are using the device for the first time or from the factory reset state.
   Enabling the Diagnostic Web Server (DWS) is recommended as it's a handy way to transfer files and check
   various things on the player. This can be done in BrightAuthor:connected when creating setup files for a new player.

0. Power off the player
1. __Enable serial control__ | Connect a serial cable from the player to your development host.  Configure your terminal program for 115200 bps, no parity, 8 data bits, 1 stop bit (n-8-1) and start the terminal program.  Hold the __`SVC`__ button while applying power. _Quick_, like a bunny, type Ctl-C in your serial terminal to get the boot menu -- you have 3 seconds to do this.  type

```bash
=> console on
=> reboot
```

2. __Reboot the player again__ using the __`RST`__ button or the _Reboot_ button from the __Control__ tab of DWS for the player.  Within the first 3 seconds after boot, again type Ctl-C in your serial terminal program to get the boot prompt and type:

```bash
=> setenv SECURE_CHECKS 0
=> envsave
=> printenv
```

Verify that `SECURE_CHECKS` is set to 0. And type `reboot`.

**The player is now unsecured.**

**🎯 Deploy to Player**:

1. Transfer extension package to BrightSign player via DWS
2. Install: `bash ./ext_npu_voice_install-lvm.sh && reboot`
3. Extension auto-starts with USB camera and microphone detection

## 📋 Requirements & Prerequisites

### Hardware Requirements

| Component | Requirement |
|-----------|-------------|
| __Development Host__ | x86_64 architecture (Intel/AMD) |
| __BrightSign Player__ | Series 5 XT-5 dev board |
| __Camera__ | USB webcam (tested: Logitech C270, Thustar) |
| __Storage__ | 25GB+ free space for builds |

### Supported Players

| Player | SOC | Platform Code | Status |
|--------|-----|---------------|---------|
| XT-5 (XT1145, XT2145) | RK3588 | XT5 | ✅ Production |

### Software Requirements

- **Docker** (for containerized builds)
- **Git** (for repository cloning)
- **25GB+ disk space** (for OpenEmbedded builds)

__Important__: Apple Silicon Macs are not supported. Use x86_64 Linux or Windows with WSL2.

## ⚙️ Configuration & Customization

The extension is highly configurable via BrightSign registry keys:

### Core Settings

```bash
# Auto-start control
registry write extension bsext-voice-disable-auto-start true

# Camera device override
registry write extension bsext-voice-video-device /dev/video1

# Disable Image stream server
registry write networking bs-image-stream-server-port 0
```

### Image Stream Server

The **BrightSign Image Stream Server** is a built-in networking feature that serves camera frames over HTTP. When this extension is running, you may want to disable the image stream server to:

- **Prevent conflicts**: Avoid multiple processes accessing the camera simultaneously
- **Optimize performance**: Reduce system load for better AI processing performance  
- **Security**: Disable unnecessary network services in production deployments

**Configuration Options:**

| Port Value | Behavior |
|------------|----------|
| `0` | **Disabled** - Image stream server is turned off (recommended for this extension) |
| `8080` | **Default** - Serves camera feed at `http://player-ip:8080/image_stream.jpg` |
| Custom port | Serves camera feed at the specified port |

**Usage Examples:**
```bash
# Disable image stream server (recommended)
registry write networking bs-image-stream-server-port 0

# Enable on default port 8080
registry write networking bs-image-stream-server-port 8080

# Enable on custom port
registry write networking bs-image-stream-server-port 9000
```

> **Note**: Changes to the image stream server port require a player reboot to take effect.

### Extension Control

This extension allows two, optional registry keys to be set to:

* Disable the auto-start of the extension -- this can be useful in debugging or other problems
* Set the `v4l` device filename to override the auto-discovered device

**Registry keys are organized in the `extension` section**

| Registry Key | Values | Effect |
| --- | --- | --- |
| `bsext-voice-disable-auto-start` | `true` or `false` | when truthy, disables the extension from autostart (`bsext_init start` will simply return). The extension can still be manually run with `bsext_init run` |
| `bsext-voice-video-device` | a valid v4l device file name like `/dev/video0` or `/dev/video1` | normally not needed, but may be useful to override for some unusual or test condition |

### Extension Behavior

- **Data Output**: UDP streaming with automatic speech recognition(ASR) results
- **Performance**: Real-time face detection and gaze estimation and voice detection on NPU

## 📊 Using the Inference Data

The extension continuously monitors the camera feed using **dual AI processing pipelines**:

**👁️ Gaze Detection Pipeline:**

- Detects all faces in the camera's field of view using RetinaFace
- Analyzes eye positioning within each detected face
- Infers attention state: faces with both eyes visible are considered "attending"

**🎤 Voice Recognition Pipeline:**

- Captures audio from connected microphone when faces are attending
- Transcribes speech to text in real-time using Whisper encoder-decoder
- Processes audio continuously for immediate response

**📡 Data Output:**
Both gaze metrics and speech transcription results are streamed via UDP to `localhost` at 1-second intervals on ports 5000 (BrightScript format) and 5002 (JSON format).

### UDP Output Formats

**Port 5000** (BrightScript format for BrightAuthor:connected):

```ini
faces_attending:1!!faces_in_frame_total:1!!ASR:"transcribed audio text"!!timestamp:1746732408
```

**Port 5002** (JSON format for node applications):

```json
{"faces_attending":1,"faces_in_frame_total":1,"ASR":"transcribed audio text","timestamp":1746732408}
```

### Output Data Fields

| Property | Description |
| --- | --- |
| `faces_in_frame_total` | Total count of all faces detected in the current frame |
| `faces_attending` | Number of faces estimated to be paying attention to the screen |
| `ASR` | Transcribed audio text |
| `timestamp` | Unix timestamp of the measurement |

### Integration Examples

- **HTML/Node.js**: [Simple Voice Detection HTML](https://github.com/brightsign/simple-voice-detection-html)

## 🖼️ Decorated Camera Output and Live ASR

Every frame of video captured is processed through the model. Every detected face has a bounding box drawn around it. Faces with two eyes detected have a green box, otherwise the box is red. The decorated image is written to `/tmp/output.jpg` on a RAM disk so it will not impact storage life.
**Live ASR ** displays the current speech transcription from the Whisper model when viewers are attending and speaking.

## 📦 Production Deployment

### Installation Methods

**Development Installation** (volatile, lost on reboot):

```bash
# On player
mkdir -p /usr/local/voice && cd /usr/local/voice
unzip /storage/sd/voice-dev-*.zip
./bsext_init run  # Test in foreground
```

**Production Installation** (permanent):

```bash
# On player
cd /usr/local && unzip /storage/sd/voice-ext-*.zip
bash ./ext_npu_voice_install-lvm.sh
reboot  # Extension auto-starts after reboot

```

## 🛠️ Development & Testing

### Rapid Development Workflow

For faster iteration during development, consider using Orange Pi boards:

__📋 See [OrangePI_Development.md](OrangePI_Development.md) for complete development guide__

Benefits:

- **Faster builds**: Native ARM compilation vs cross-compilation
- **Better debugging**: Full GDB support and system monitoring
- **Same hardware**: Uses identical Rockchip SoCs as BrightSign players

### Troubleshooting

**Common Issues**:

- **Docker not running**: `systemctl start docker`
- **Permission denied**: Add user to docker group
- **Out of space**: Need 25GB+ for OpenEmbedded builds
- __Wrong architecture__: Must use x86_64 host (not ARM/Apple Silicon)

### Multi-Platform Development

The extension automatically detects platform at runtime:

- **RK3588** (XT-5): Uses `RK3588/` subdirectory, `/dev/video1`

## 📚 Technical Documentation

For in-depth technical information:

### 📄 [Manifest Guide](docs/manifest-guide.md)

- Extension versioning system
- Compatibility declarations
- User configuration options

### 🍊 [Orange Pi Development](OrangePI_Development.md)

- Rapid prototyping workflow
- Native development environment
- Testing and debugging guide

### 🛠️ [Development Guide](docs/development-guide.md)

- Development workflow and best practices
- Testing strategies
- Debugging techniques

## 🗑️ Removing the Extension

To remove the extension, you can perform a Factory Reset or remove the extension manually:

1. Connect to the player over SSH and drop to the Linux shell
2. STOP the extension: `/var/volatile/bsext/ext_npu_voice/bsext_init stop`
3. VERIFY all the processes for your extension have stopped
4. Run the uninstall script: `/var/volatile/bsext/ext_npu_voice/uninstall.sh`
5. Reboot to apply changes: `reboot`

---

For questions or issues, see the troubleshooting section or check the technical documentation.
