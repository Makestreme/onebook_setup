# OneBook setup guide

This guide covers the **software side**: Cloudflare, the firmware, and connecting the web app. It's written for beginners, with instructions for **Windows, macOS and Linux**.

> **Building the hardware?** Wiring, parts and assembly are covered in the [OneBook build guide on Instructables](INSTRUCTABLES-LINK). Finish that first, then come back here.

| Step | What you do | Time |
|---|---|---|
| 1 | Check you have everything | 5 min |
| 2 | Download the OneBook files | 2 min |
| 3 | Prepare the microSD card | 5 min |
| 4 | Create two secret passwords | 2 min |
| 5 | Put your tunnel on Cloudflare | 15 min |
| 6 | Install the firmware on OneBook | 20 min |
| 7 | First boot and Wi-Fi setup | 5 min |
| 8 | Connect the web app | 2 min |
| 9 | Add books | |

The rest of the guide covers day-to-day changes, troubleshooting, and technical reference.

---

## 1. What you need
- **A built OneBook**, following the [build guide](INSTRUCTABLES-LINK).
- **A microSD card** and a way to plug it into your computer (built-in slot or USB card reader).
- **A computer** running Windows 10/11, macOS 12 or newer, or a desktop Linux such as Ubuntu or Fedora. It's only needed for setup.
- **A phone** with Wi-Fi, for the first-boot setup.
- **A free Cloudflare account.** You'll create it in step 5.
- **Two free programs** you'll install along the way: **Node.js** and the **Arduino IDE**.

### A word about the terminal
A few steps ask you to type commands into a **terminal**: a window where you type instructions instead of clicking.

| | How to open it |
|---|---|
| **Windows** | Press the Start button, type **PowerShell**, press Enter. |
| **macOS** | Press ⌘ + Space, type **Terminal**, press Enter. |
| **Linux** | Press Ctrl + Alt + T, or find **Terminal** in your app menu. |

How to use it:
- Type or paste a command, then press **Enter**.
- To paste: Windows PowerShell uses **right-click** or Ctrl + V. macOS uses ⌘ + V. Linux uses **Ctrl + Shift + V**.
- Commands in this guide are shown in grey boxes like this:
  ```
  node -v
  ```

---

## 2. Download the OneBook files
1. Open **https://github.com/makestreme/onebook_setup** and click the green **Code** button, then **Download ZIP**.
2. Extract the ZIP. You'll get a folder containing `OneBook`, `worker` and these guides.
   - **Windows:** right-click the ZIP → **Extract All**.
   - **macOS:** double-click the ZIP.
   - **Linux:** right-click → **Extract Here**, or run `unzip <file>.zip`.
3. Move the extracted folder somewhere easy to find, such as your **Documents** folder.

> Using git instead? Run `git clone https://github.com/makestreme/onebook_setup.git`; the folder layout is the same.

---

## 3. Prepare the microSD card
OneBook needs the card formatted as **FAT32**; exFAT and NTFS will not work. **Formatting erases everything on the card.**

**Windows**
- **32 GB or smaller:**
  1. Insert the card and open **File Explorer**.
  2. Right-click the card → **Format**.
  3. Choose File system: **FAT32**, then click **Start**.
- **Larger than 32 GB:** Windows only offers exFAT for these. Use a free FAT32 formatting tool, such as *FAT32 Format (guiformat)*.

**macOS**
1. Open **Disk Utility**: press ⌘ + Space and type "Disk Utility".
2. Choose **View → Show All Devices**, then select the card itself (the top-level entry, not the volume under it).
3. Click **Erase** and choose:
   - Format: **MS-DOS (FAT)**
   - Scheme: **Master Boot Record**
4. Click **Erase**.

**Linux**
1. Open **Disks** (gnome-disks) and select the card.
2. Click the gear icon → **Format Partition** → type **"For use with all systems and devices (FAT)"**.
3. Click **Format**.

Using the terminal instead? Find the card with `lsblk`, then run `sudo mkfs.vfat -F 32 /dev/sdX1`, replacing `sdX1` with your card's partition. Double-check the name: picking the wrong disk erases it.

**Then:**
1. Optionally create a folder called `books` on the card and copy your `.epub` files into it. OneBook creates the folder itself if it's missing, and you can upload books from the web app later instead.
   - Plain filenames are safest: letters, digits, spaces and `. _ - ( ) [ ] , ' & + !`.
   - Books copied this way get their cover and title the first time you open them in the web app.
2. Eject the card safely and put it in OneBook.

---

## 4. Create two secret passwords
OneBook uses two long random passwords. Nobody has to remember them; you'll copy and paste them.

| Name in this guide | What it protects | You'll paste it into |
|---|---|---|
| **Device secret** | Only *your* OneBook can connect to your tunnel | Cloudflare (step 5) and OneBook's setup page (step 7) |
| **Access token** | Only *your* apps can open your library | Cloudflare (step 5) and the web app (step 8) |

Open a terminal and run the command for your system **twice**. Each run prints a new 60-character secret; use one for each.

**Windows (PowerShell)**
```powershell
$b = New-Object byte[] 30; [Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($b); -join ($b | ForEach-Object { $_.ToString('x2') })
```

**macOS and Linux**
```bash
openssl rand -hex 30
```

Save both in a password manager or a private note, clearly labelled. **Never use the same value for both.**

---

## 5. Put your tunnel on Cloudflare
Your **Worker** is a tiny program that runs on Cloudflare for free. It gives OneBook a permanent web address such as `https://onebook.alex.workers.dev`:
- Your web app sends requests to that address.
- OneBook keeps a connection open to it from inside your home.
- The Worker passes messages between the two.

You'll use two tools to set it up:
- **Node.js and npm.** Node.js is a program that runs JavaScript tools on your computer. npm is its tool installer and comes with it. You only need them for setup.
- **Wrangler.** Cloudflare's own command-line tool for uploading Workers. You don't install it separately: the `npx` command (also part of npm) downloads and runs it for you.

### 5.1 Create a Cloudflare account
1. Go to **https://dash.cloudflare.com/sign-up** and sign up with your email. No payment card is needed.
2. Confirm your email address using the link Cloudflare sends.
3. You don't need to add a website or domain. Skip any prompts about that.

### 5.2 Install Node.js
OneBook needs **Node.js 20 or newer**; the current LTS ("long-term support") version is best.

**Windows**
1. Go to **https://nodejs.org**, download the **LTS** installer (`.msi`) and run it.
2. Accept the defaults and click through to **Finish**. It installs npm and sets everything up.
3. Alternatively, in PowerShell: `winget install OpenJS.NodeJS.LTS`

**macOS**
1. Go to **https://nodejs.org**, download the **LTS** macOS installer (`.pkg`) and run it.
2. Alternatively, if you use Homebrew: `brew install node`

**Linux**
- Your distribution's own package is often too old. Use the official instructions at **https://nodejs.org/en/download**: pick *Linux* and the method shown (usually **nvm**), and copy the commands it gives you.
- On Fedora, `sudo dnf install nodejs` normally provides a recent enough version.

**Check it worked.** Close any open terminal, open a **new** one, and run:
```
node -v
npm -v
```
You should see version numbers, for example `v22.x.x` and `10.x.x`. If you get *"command not found"* or *"not recognized"*, restart your computer and try again.

### 5.3 Open a terminal in the `worker` folder
All Cloudflare commands must run inside the `worker` folder from step 2.

| | How |
|---|---|
| **Windows 11** | Open the `worker` folder in File Explorer, right-click an empty area → **Open in Terminal**. |
| **Windows 10** | Open the `worker` folder in File Explorer, click the address bar, type `powershell` and press Enter. |
| **macOS** | Open Terminal, type `cd ` (with a space after it), drag the `worker` folder onto the Terminal window, then press Enter. |
| **Linux** | Right-click inside the `worker` folder → **Open in Terminal**, or `cd` to it, e.g. `cd ~/Documents/onebook/worker`. |

Check you're in the right place. You should see `worker.js` and `wrangler.toml` listed:
- **Windows:** `dir`
- **macOS/Linux:** `ls`

### 5.4 Check `wrangler.toml`
Open `wrangler.toml` in a plain text editor:
- **Windows:** Notepad.
- **macOS:** TextEdit. Choose **Format → Make Plain Text** before saving.
- **Linux:** Text Editor or gedit.
- Or VS Code on any system.

Only two lines matter:

| Line | What to do |
|---|---|
| `name = "onebook"` | The first part of your address. Keep it, or change it to any lowercase name, e.g. `mybooks`. |
| `ALLOWED_ORIGIN = "https://makestreme.github.io"` | **Leave it as is.** It's the address of the OneBook web app, and browsers won't let any other website use your Worker. |

Save the file if you changed anything.

### 5.5 Log in to Cloudflare from the terminal
```
npx wrangler login
```
1. The first time, npx asks to install Wrangler with *"Need to install the following packages: wrangler … Ok to proceed? (y)"*. Type **y** and press Enter. It takes a minute.
2. Your browser opens a Cloudflare page. Log in if asked, then click **Allow**.
3. The terminal shows *"Successfully logged in"*.

To check which account you're logged in to at any time:
```
npx wrangler whoami
```

> **Linux without a browser on the same machine (e.g. over SSH)?** The login page can't call back to the terminal. Instead:
> 1. In the Cloudflare dashboard, go to **My Profile → API Tokens → Create Token** and use the **"Edit Cloudflare Workers"** template.
> 2. Run `export CLOUDFLARE_API_TOKEN=<the token>` in the terminal.
> 3. Skip `wrangler login`.

### 5.6 Upload (deploy) the Worker
```
npx wrangler deploy
```
- **First Worker ever on this account:** Wrangler asks you to choose a **workers.dev subdomain**, for example `alex`. Pick something short; it becomes part of every Worker address you create.
  - If it doesn't ask and stops with an error about a subdomain, open the Cloudflare dashboard → **Workers & Pages**. Click the link to set up your `workers.dev` subdomain, then run the deploy command again.
- **When it finishes,** it prints your Worker's address, for example:
  ```
  https://onebook.alex.workers.dev
  ```
  **Write this down.** It's your *Worker address* for steps 7 and 8.

### 5.7 Store the two secrets on Cloudflare
Run each command. When it asks *"Enter a secret value"*, paste the matching secret from step 4 and press Enter. Nothing appears on screen while you paste; that's normal.
```
npx wrangler secret put DEVICE_SECRET
npx wrangler secret put APP_TOKEN
```
- `DEVICE_SECRET` is your **device secret**.
- `APP_TOKEN` is your **access token**.
- Each one should end with *"Success! Uploaded secret …"*.

Prefer clicking? In the Cloudflare dashboard go to **Workers & Pages → onebook → Settings → Variables and Secrets → Add**. Choose type **Secret** and use the same names, `DEVICE_SECRET` and `APP_TOKEN`.

### 5.8 Check your Worker
Open your Worker address in any browser. You should see:
```
OneBook tunnel is running.
```
Your tunnel is live, and it stays running on Cloudflare even when your computer is off.

### 5.9 Later: updates and logs
- **Update the Worker:** download the new files, open a terminal in the `worker` folder again, and run `npx wrangler deploy`. Your secrets are kept. OneBook disconnects for a few seconds and reconnects by itself.
- **Watch live activity**, useful when troubleshooting:
  ```
  npx wrangler tail
  ```
  Press Ctrl + C to stop.

### 5.10 If something goes wrong here
| Message | Fix |
|---|---|
| Windows: *"npx.ps1 cannot be loaded because running scripts is disabled on this system"* | Run `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned` in PowerShell and answer **Y**. Or use **Command Prompt** (`cmd`) instead of PowerShell. |
| *"npx: command not found"* / *"'npx' is not recognized"* | Node.js isn't installed or the terminal was open during install. Open a new terminal or restart, then check `node -v`. |
| *"Wrangler requires at least Node.js v…"* | Install the current LTS from nodejs.org (step 5.2). |
| *"Could not find wrangler.toml"* or *"Missing entry-point"* | You're not in the `worker` folder. Repeat step 5.3. |
| Login page says the request failed or timed out | Run `npx wrangler login` again. Behind a strict company network or VPN, try another network or use the API token method in 5.5. |
| *"You need to register a workers.dev subdomain"* | Dashboard → **Workers & Pages** → set up your subdomain, then deploy again. |
| Linux/macOS: *"EACCES: permission denied"* | Don't use `sudo` with npx. Installing Node.js with nvm (5.2) avoids permission problems. |

---

## 6. Install the firmware on OneBook
The firmware is the program that runs on OneBook itself. You'll use the free **Arduino IDE** to compile it and copy it onto the board.

### 6.1 Install the Arduino IDE
Download **Arduino IDE 2.x** from **https://www.arduino.cc/en/software**.

| | How |
|---|---|
| **Windows** | Run the installer and accept the defaults. Allow the driver installs if asked. |
| **macOS** | Open the `.dmg` and drag Arduino IDE into **Applications**. |
| **Linux** | Download the **AppImage**, right-click it → Properties → allow executing as a program, or run `chmod +x arduino-ide_*.AppImage`. Double-click to start. |

**Linux only: allow access to USB serial ports.** Run this once, then **log out and back in**:
```bash
sudo usermod -a -G dialout $USER
```
On Arch-based systems the group is `uucp` instead of `dialout`.

### 6.2 Add ESP32 support (board package version 2.0.18)
1. Open the Arduino IDE. Go to **File → Preferences** (on macOS: **Arduino IDE → Settings**).
2. In **Additional boards manager URLs**, paste:
   ```
   https://espressif.github.io/arduino-esp32/package_esp32_index.json
   ```
   Then click **OK**.
3. Open **Tools → Board → Boards Manager** and search for **esp32**.
4. On **esp32 by Espressif Systems**, pick version **2.0.18** from the drop-down and click **Install**. The download is several hundred MB.

> **Use 2.0.18, not 3.x.** The firmware is written for the 2.x version, and 3.x will not compile it. If 3.x is already installed, select 2.0.18 and install it over the top.

### 6.3 Install the three libraries
Go to **Sketch → Include Library → Manage Libraries…**, search for each name, and click **Install**. If it offers to install dependencies, choose **Install All**.

| Search for | Pick the one by | Version |
|---|---|---|
| **WiFiManager** | tzapu | 2.0.17 or newer |
| **WebSockets** | Markus Sattler | 2.7.x |
| **ArduinoJson** | Benoit Blanchon | 7.x |

Everything else the firmware uses comes with the ESP32 package.

### 6.4 Open the firmware
1. In the Arduino IDE, go to **File → Open** and choose `OneBook/OneBook.ino`. Keep the file inside a folder named `OneBook`; the Arduino IDE requires that.
2. Optional settings near the top of the file:

| Setting | Default | Change it if |
|---|---|---|
| `PORTAL_PASS` | `onebook-setup` | You want your own password for the setup hotspot (8+ characters) |
| `PIN_…` lines | Standard ESP32-S3 pins (see below) | Your board or wiring uses different pins |
| `DEVICE_HOSTNAME` | `onebook` | You want a different name than `onebook.local` |

You don't need to enter any secrets or addresses in the code. OneBook asks for them on its setup page (step 7).

**Pins.** The defaults are the standard ESP32-S3 pins, which suit most boards:

| Line in the code | Default GPIO | Can be |
|---|---|---|
| `PIN_SD_MOSI` | 11 | Your board's SPI **MOSI** pin |
| `PIN_SD_MISO` | 13 | Your board's SPI **MISO** pin |
| `PIN_SD_SCK` | 12 | Any free GPIO (12 is the standard SPI clock) |
| `PIN_SD_CS` | 10 | Any free GPIO (10 is the standard SPI chip select) |
| `PIN_LED` | 48 | Any free GPIO for the WS2812 data line |

- **Finding MOSI and MISO for your board:** search online for *"<your board name> pinout"*. They may be labelled **FSPID** (MOSI) and **FSPIQ** (MISO).
- **Built-in RGB LED:** many ESP32-S3 boards already have a WS2812 on GPIO **48**, and some (such as the DevKitC-1 v1.1) on GPIO **38**. Set `PIN_LED` to it and you don't need a separate LED.
- **Pins to avoid:** 0, 3, 45 and 46 (they control how the chip boots), 19 and 20 (USB), 26–37 (used by the flash and PSRAM on many boards), and 43 and 44 (the serial port used for uploading).

### 6.5 Choose the board settings
In the **Tools** menu, set:

| Setting | Value |
|---|---|
| Board | **ESP32S3 Dev Module** (under *esp32*) |
| Flash Size | **8MB (64Mb)** |
| Partition Scheme | **8M with spiffs (3MB APP/1.5MB SPIFFS)** |
| PSRAM | **Disabled** |
| USB CDC On Boot | **Disabled** |
| Upload Mode | **UART0 / Hardware CDC** |
| Core Debug Level | **None** |

### 6.6 Connect OneBook and pick the port
1. Connect OneBook to your computer as described in the [build guide](INSTRUCTABLES-LINK).
2. Open **Tools → Port** and pick the port that appeared when you plugged in:

| | Port looks like |
|---|---|
| **Windows** | `COM3`, `COM4`, … |
| **macOS** | `/dev/cu.usbserial-…` or `/dev/cu.wchusbserial…` |
| **Linux** | `/dev/ttyUSB0` or `/dev/ttyACM0` |

**No new port appears?**
- The USB-serial adapter may need a driver. Most use a **CP210x** chip (driver from Silicon Labs) or a **CH340** chip (driver from WCH).
  - Windows usually installs these automatically.
  - macOS may need the CH340 driver.
  - Linux has them built in.
- Also try a different USB cable; some cables are charge-only.

### 6.7 Upload
1. Put OneBook into **download mode** as described in the build guide (usually: hold **BOOT**, tap **RST**, release **BOOT**).
2. Click the **Upload** button (the right-arrow icon) in the Arduino IDE.
   - The first compile takes a few minutes; later ones are faster.
   - When the bottom panel shows *"Hard resetting via RTS pin…"* or *"Done uploading"*, it's finished.
3. Finish as the build guide says (for example, disconnect the programming adapter), then press **RST** or power-cycle OneBook.

If compiling fails, copy the **entire** error text from the bottom panel when asking for help.

---

## 7. First boot and Wi-Fi setup
1. Power OneBook on. The LED **blinks white** while it looks for saved Wi-Fi. On a first boot there is none, so it goes straight to the next step.
2. The LED turns **blue**: the setup hotspot is open.
3. On your phone, join the Wi-Fi network **`OneBook-Setup`** (password `onebook-setup`, or your own `PORTAL_PASS`). A setup page opens automatically. If it doesn't, browse to `http://192.168.4.1`.
4. Tap **Configure WiFi** and fill in:
   - **SSID / password**: your home Wi-Fi.
   - **Worker host**: your Worker address, for example `onebook.alex.workers.dev`. Pasting it with `https://` is fine.
   - **Device secret**: the device secret from step 4.
5. Tap **Save**. OneBook joins your Wi-Fi and connects to the Worker.
   - **Purple**: Wi-Fi is fine; it's still connecting to the Worker. A few seconds is normal.
   - **Off**: all good, OneBook is online.
6. Optional check: on the same network, open **http://onebook.local** (or the IP your router shows). This read-only page shows firmware, SD usage, the book list, and a live log.

### 7.1 Changing settings later (no buttons needed)
- **New Wi-Fi network or password:** if OneBook can't connect to its saved Wi-Fi for 60 seconds, it opens the `OneBook-Setup` hotspot by itself. The hotspot stays open for 5 minutes, then OneBook tries the saved network again, and repeats until one of them succeeds.
- **New Worker host or device secret, while Wi-Fi works:** power-cycle twice quickly.
  1. Plug in and wait about 1 second.
  2. Unplug.
  3. Plug back in within 3 seconds.

  The hotspot opens. Leave the device secret blank to keep the current one. Pressing **RST** twice within 3 seconds does the same.
- **Wi-Fi drops for more than 5 minutes:** OneBook restarts and goes through the steps above.

### 7.2 LED meanings
| LED | Meaning |
|---|---|
| Off | Online and idle |
| Blinking white | Connecting to Wi-Fi |
| Blue | Setup hotspot open |
| Purple | No connection to the Worker |
| Amber | Transferring a book |
| Green flash | Bookmark added, renamed or deleted |
| Red | SD card not available |

### 7.3 Rotating secrets
- **Access token:** run `npx wrangler secret put APP_TOKEN` with a new value, then enter it in the web app's Settings on each device.
- **Device secret:** run `npx wrangler secret put DEVICE_SECRET` with a new value, then power-cycle OneBook twice to open the hotspot and enter it.

### 7.4 Certificate check
OneBook verifies the Worker's TLS certificate against the root certificates built into the sketch (`CA_BUNDLE`). These cover every authority Cloudflare uses for free certificates: Google Trust Services, Let's Encrypt and SSL.com. As a last resort, setting `#define TLS_VERIFY 0` keeps the connection encrypted but skips the check.

### 7.5 Serial log
OneBook prints its log over serial at 115200 baud. With OneBook connected as for uploading, open **Tools → Serial Monitor** in the Arduino IDE and set it to 115200 baud.

The same log is also at `http://onebook.local` and in the web app's **Settings → Show device log**.

---

## 8. Connect the web app
1. Open **https://makestreme.github.io/onebook/** on your phone or computer.
2. Settings opens on first use. Enter:
   - **Server address**: your Worker address, for example `https://onebook.alex.workers.dev`.
   - **Access token**: the access token from step 4.
3. Tap **Test connection**. You should see *Connected to OneBook 1.0.0* and your storage use.
4. Tap **Save**.
5. Optional: on Android/Chrome use **Add to Home screen**, or on desktop use **Install**, for an app-like experience.

Repeat on every device you read on. Each browser stores its own settings and downloaded books.

---

## 9. Add books
- **From the web app:** tap **Upload** and choose or drop EPUB files (max 20 MB each).
  - The app reads the title, author and cover before uploading.
  - It renames files to names the device accepts if needed, and tells you the new name.
- **From a computer:** copy files into `/books` on the SD card.

Delete books from a book's **⋮** menu → **Delete from OneBook**. This also removes its bookmarks.

---

## 10. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| LED red | SD card not mounted | Is the card fully inserted? Is it FAT32, not exFAT (section 3)? The log shows `SD: mount failed`, and OneBook retries every 10 s. Still red: check the SD wiring against the [build guide](INSTRUCTABLES-LINK). |
| LED purple for more than a minute | Can't reach the Worker | Check the Worker host spelling (no extra path). Make sure the device secret matches `DEVICE_SECRET` exactly. Open the Worker address in a browser. Check the log for `Tunnel: connect failed`. |
| LED blinking white, then blue | Saved Wi-Fi not found | Fix it through the hotspot (section 7). |
| Web app: *Access denied* | Wrong access token | Re-enter `APP_TOKEN` in Settings. |
| Web app: *OneBook is offline* | Worker is up, device isn't connected | See *LED purple*. Is OneBook powered? |
| Web app: *Cannot reach the server* | Wrong address, or browser blocked it | Check the address. The browser console shows a CORS error if `ALLOWED_ORIGIN` in `wrangler.toml` doesn't match the web app's address; fix it and redeploy. |
| Upload: *Not enough space* | Fewer than 50 MB would be left free | Delete books or use a bigger card. |
| *Could not open this book* | Damaged or DRM-protected EPUB | Only DRM-free EPUBs work. |
| Transfers feel slow | Normal tunnel speed | Roughly 100-400 KB/s. Books are cached after the first open. |
| Compile errors | Wrong board package or a missing library | Make sure **esp32 2.0.18** is installed, not 3.x (section 6.2), and all three libraries are installed (6.3). |
| *"Sketch too big"* / *"text section exceeds available space"* | Partition Scheme still on the 4 MB default (output says *Maximum is 1310720 bytes*) | Set **Flash Size: 8MB** first, then **Partition Scheme: 8M with spiffs (3MB APP/1.5MB SPIFFS)** (section 6.5). If that option isn't listed, **Huge APP (3MB No OTA/1MB SPIFFS)** also works. |
| *"Failed to connect to ESP32"* when uploading | Board not in download mode, or wrong port | Hold BOOT, tap RST, release BOOT, then upload again. Check **Tools → Port** (section 6.6). |
| No port shows up | Missing USB driver, charge-only cable, or (Linux) no permission | See section 6.6. On Linux, run the `dialout` command from 6.1 and log out and back in. |
| Works while connected for uploading, not afterwards | Programming connection still attached | Finish as described in the [build guide](INSTRUCTABLES-LINK), then press RST. |
| Bookmarks didn't sync | Device was offline | They sync automatically next time OneBook is reachable. Tap ⟳ (Refresh) to retry now. |

---

## 11. Cloudflare free plan usage
| Free plan limit | Typical OneBook use |
|---|---|
| 100,000 Worker requests / day | A few hundred |
| 100,000 Durable Object requests / day (WebSocket messages count 1/20; pings are free) | A few hundred |
| 13,000 GB-s Durable Object duration / day | Near zero: the Durable Object sleeps between requests |

If a free limit were ever exceeded, requests would fail until the daily reset (00:00 UTC). Nothing is charged.

---

## 12. Technical reference

### 12.1 Device storage layout
| Path | Storage | Contents |
|---|---|---|
| `/books/<name>.epub` | microSD | Books |
| `/books/<name>.epub.part` | microSD | Upload in progress (cleaned up at boot) |
| `/.onebook/meta/<hash>.json` | microSD | `{"v":1,"title","author","cover":"data:image/jpeg;base64,..."}` (≤ 64 KB) |
| `/bm/<hash>.json` | Internal flash (LittleFS) | Bookmark document (≤ 16 KB) |

`<hash>` is the 64-bit FNV-1a hash of the lowercased filename, written as 16 hexadecimal characters.

### 12.2 Bookmark document
```json
{
  "v": 1,
  "last":  { "cfi": "epubcfi(/6/4!/4/2/1:0)", "pct": 0.169, "label": "Chapter 2", "at": 1791544837013 },
  "marks": [
    { "id": "mv0vhx90219lk7", "name": "Where the storm starts", "cfi": "epubcfi(/6/2!/4/20/1:257)",
      "pct": 0.039, "label": "Chapter 1", "at": 1791544855746, "c": 1791544766868 },
    { "id": "mv0vjg5c1v13fo", "del": true, "at": 1791544856196, "c": 1791544838016 }
  ]
}
```
How merging works:
- The web app merges documents with "latest timestamp wins", per bookmark id and for `last`.
- A deleted bookmark is kept as a small "deleted" record for 90 days, so the deletion isn't undone by an older copy on another device.

### 12.3 HTTP API (Worker)
Every request needs `Authorization: Bearer <access token>`. Book names in paths are URL-encoded.

| Method and path | Description |
|---|---|
| `GET /api/status` | Firmware, uptime, free heap, Wi-Fi RSSI, SD total/used |
| `GET /api/books` | `{"books":[{"n":name,"s":size,"m":hasMeta}]}` |
| `GET /api/log` | Recent device log (plain text) |
| `GET /api/books/<name>` | Download a book |
| `PUT /api/books/<name>` | Upload a book (body = file, `Content-Length` required, ≤ 20 MB) |
| `DELETE /api/books/<name>` | Delete a book, its metadata and bookmarks |
| `GET` / `PUT /api/meta/<name>` | Book metadata JSON |
| `GET` / `PUT /api/bookmarks/<name>[?flash=1]` | Bookmark document (`flash=1` makes the LED flash green) |

Errors are returned as `{"error":"message"}`. Worker-specific statuses:

| Status | Meaning |
|---|---|
| `401` | Bad token |
| `503` | OneBook is offline |
| `504` | No response for 45 s |

### 12.4 Tunnel protocol (Worker to device)
The device connects to `wss://<worker>/device` with the header `Authorization: Bearer <device secret>`.

| Direction | Message |
|---|---|
| Worker → device (text) | `{"t":"req","id":N,"m":"GET\|PUT\|DELETE","p":"/api/...","len":bodyBytes}` |
| Worker → device (text) | `{"t":"abort","id":N}`: the browser went away or timed out |
| Device → Worker (text) | `{"t":"hello","fw":"1.0.0"}` on connect |
| Device → Worker (text) | `{"t":"res","id":N,"s":status,"ct":"content/type","len":bodyBytes}` |
| Device → Worker (text) | `{"t":"fail","id":N}`: body stream broke after `res` |
| Both directions (binary) | 4-byte big-endian request id + up to 8192 body bytes |

How exchanges run:
- **One request at a time.** The Durable Object queues the rest (up to 32).
- **Early answers.** The device may answer before a request body has fully arrived, for example to reject an upload. The Worker then stops sending that body.
- **Keep-alive.** The device sends WebSocket pings every 20 s. Cloudflare answers them without waking the Durable Object.
