# Setting up cloud upload

The camera can copy its photos and recordings to **your own Google Drive or
Dropbox**. Nothing goes through anyone else's server: you make a small "app" in
your own account that gives the camera permission, and the camera stores the key
it is given.

You do this **once per service**. It takes about 10 minutes. Pick one:

- [Google Drive](#google-drive)
- [Dropbox](#dropbox)

Then finish with [Set it up on the camera](#set-it-up-on-the-camera).

> Google and Dropbox change their web pages from time to time. If a button is
> named a little differently from the one below, look for the closest match. The
> steps and the names of the things you create stay the same.

## Which cameras

Cloud upload is built into the Raspberry Pi, SIYI A8 mini, SIYI ZR10, SIYI ZT30, SIYI MT11
and XFRobot Z1-Mini firmware. The set-up below is the same on all of them.

| Camera | Tested on the camera | Credentials and remembered files | CA certificates |
|---|---|---|---|
| Raspberry Pi | yes | `/etc/ap_cameragimbal` | `/usr/local/share/ap_cameragimbal/ca-certificates.pem` |
| A8 mini | not yet | `/config/camera-app` | in the package (`/customer/camera-app`) |
| Z1-Mini | not yet | `/opt/ap_cameragimbal` | built into the program |
| ZR10 | not yet | the settings folder on the SD card | in the package (on the SD card) |
| MT11 | not yet | `/app` | in the package (`/app`) |
| ZT30 | not yet | `/customer/camera-app` | built into the program |

The simulators (`make sitl`) can upload too, so the set-up can be tried without a camera. They keep the credentials
in the simulator's `app` folder (`rclone.conf`, under `build/<camera>-sitl/runtime`) and trust this computer's own
CA certificates.

## Before you start

You need:

- the camera running and reachable from your computer. Its web page is at
  `http://<camera address>:8080/` (for example `http://192.168.144.20:8080/`),
  and you sign in as `admin`;
- a computer with a web browser;
- **rclone** on that computer. The camera does not use rclone. You run it once
  to get your key. Install it from <https://rclone.org/install/>
  (Windows: `winget install Rclone.Rclone`, macOS: `brew install rclone`,
  Linux: `sudo apt install rclone`). Check it works by running `rclone version`.

Two things that go wrong again and again, so read them now:

1. **Replace the capitals.** When these instructions show something like
   `"CLIENT_ID"` or `"APP_KEY"`, you must put your own value there. Typing the
   words themselves gives errors such as *"The OAuth client was not found"*.
2. **Treat the secret and the token like passwords.** Do not post them or commit
   them anywhere. If one leaks, remove access (see
   [Removing access](#removing-access)) and make a new one.

---

## Google Drive

You will make: a **project**, an **OAuth client** (a client ID and a client
secret), and a **token**.

### 1. Create a Google Cloud project

1. Go to <https://console.cloud.google.com/> and sign in with the Google account
   whose Drive you want to use.
2. Click the project name at the top left, then **New project**.
3. Name it, for example `AP_CameraGimbal`, and click **Create**.
4. Make sure the new project is the one shown at the top of the page.

<!-- screenshot: google-01-new-project.png -->

### 2. Turn on the Google Drive API

1. Open the menu (three lines, top left), then **APIs & Services**, then
   **Library**.
2. Search for **Google Drive API**, open it, and click **Enable**.

<!-- screenshot: google-02-enable-drive-api.png -->

### 3. Set up the consent screen

This is the page Google shows when you allow the camera to use your Drive.

1. Open **APIs & Services**, then **OAuth consent screen**. (Newer accounts see
   this as **Google Auth Platform**, with **Branding**, **Audience**, **Clients**
   and **Data Access** in the left menu.)
2. Click **Get started** if it is offered.
3. Enter an app name (for example `AP_CameraGimbal`) and your email address as the
   support email.
4. For the audience choose **External**.
5. Enter your email address as the contact, agree to the policy, and click
   **Create**.

<!-- screenshot: google-03-consent-screen.png -->

### 4. Publish the app (do not skip this)

This is the most common mistake. While the app is in **Testing**, Google
**expires your key after 7 days** and uploads stop with the message *"the token
has expired or been revoked"*.

1. Open **Audience** (or **OAuth consent screen**).
2. Under **Publishing status**, click **Publish app** and confirm.
3. The status should now read **In production**.

Google may show a warning that the app is "unverified". That is expected for a
personal app. You will click past it once, in step 6.

<!-- screenshot: google-04-publish-app.png -->

### 5. Create the client ID and secret

1. Open **APIs & Services**, then **Credentials** (or **Clients** in Google Auth
   Platform), and click **Create client** (or **Create credentials**, then
   **OAuth client ID**).
2. For **Application type** choose **Desktop app**. Not "Web application": that
   type fails later with a redirect error.
3. Give it a name, for example `camera`, and click **Create**.
4. Google shows a **Client ID** and a **Client secret**. Copy both now, using the
   copy buttons, and save them somewhere private. Google may show the secret only
   this once. If you lose it, open the client and add a new secret.

The client ID ends in `.apps.googleusercontent.com`; the secret starts with
`GOCSPX-`.

<!-- screenshot: google-05-create-desktop-client.png -->

### 6. Get your token with rclone

On your computer, open a terminal and run the command below, **with your own client
ID and secret** in place of the capitals, keeping the quote marks:

```sh
rclone authorize "drive" "CLIENT_ID" "CLIENT_SECRET"
```

1. rclone prints a link starting with `http://127.0.0.1:53682/auth`. Your browser
   usually opens it by itself. If not, copy the link into your browser.
2. Sign in to Google and choose your account.
3. If you see "Google hasn't verified this app", click **Advanced**, then
   **Go to AP_CameraGimbal (unsafe)**. That is your own app.
4. Click **Continue** or **Allow** to give access to your Google Drive.
5. Go back to the terminal. rclone prints:

   ```
   Paste the following into your remote machine --->
   {"access_token":"ya29.…","token_type":"Bearer","refresh_token":"1//…","expiry":"…"}
   <---End paste
   ```

6. Copy the **single line** that starts with `{` and ends with `}`, not the two
   marker lines. This is your **token**. It must contain `refresh_token`.

Now go to [Set it up on the camera](#set-it-up-on-the-camera).

---

## Dropbox

You will make: an **app** (an app key and an app secret) with the right
**permissions**, and a **token**. The camera is only allowed to use one folder,
`Apps/<your app name>`, in your Dropbox, and nothing else.

### 1. Create the app

1. Go to <https://www.dropbox.com/developers/apps> (the **App Console**) and sign
   in.
2. Click **Create app**.
3. Choose **Scoped access**.
4. Choose **App folder**.
5. Give it a name that no one else has used, for example
   `AP_CameraGimbal-yourname`, and click **Create app**.

<!-- screenshot: dropbox-01-create-app.png -->

### 2. Set the permissions (before you get the token)

Do this **before** step 4. A token only has the permissions the app had when the
token was made.

1. Open the **Permissions** tab.
2. Tick **`files.metadata.read`**.
3. Tick **`files.content.write`**.
4. Click **Submit** at the bottom of the page. Ticking the boxes alone does
   nothing until you Submit. Dropbox confirms that the scopes were updated.

<!-- screenshot: dropbox-02-permissions.png -->

If you change permissions later, you must get a new token (step 4).

### 3. Add the redirect address and copy the key

1. Open the **Settings** tab.
2. Under **OAuth 2**, find **Redirect URIs**, add `http://localhost:53682/` and
   click **Add**. rclone listens on this address to receive the sign-in.
3. Copy the **App key**, and click **Show** next to **App secret** and copy that
   too.

<!-- screenshot: dropbox-03-settings.png -->

### 4. Get your token with rclone

On your computer, run this with **your own app key and secret** in place of the
capitals, keeping the quote marks:

```sh
rclone authorize "dropbox" "APP_KEY" "APP_SECRET"
```

1. Open the link rclone prints (`http://127.0.0.1:53682/auth…`) if your browser
   does not open it.
2. Sign in to Dropbox and click **Allow**.
3. rclone prints a block like:

   ```
   Paste the following into your remote machine --->
   {"access_token":"sl.u.…","token_type":"bearer","refresh_token":"…","expiry":"…"}
   <---End paste
   ```

4. Copy the **single line** from `{` to `}`. This is your **token**. It must
   contain `refresh_token`.

Do not use the **Generate access token** button in the App Console. It makes a
token that stops working after a few hours.

Now go to [Set it up on the camera](#set-it-up-on-the-camera).

---

## Set it up on the camera

1. Open the camera's web page and go to **Parameters**, then the **Cloud Upload**
   tab.
2. Set **Cloud service** to Google Drive or Dropbox. The credentials for that
   service appear underneath.
3. Fill in the three boxes:
   - Google Drive: **Client ID**, **Client secret** and **Token**.
   - Dropbox: **App key**, **App secret** and **Token**.
4. Click **Save credentials**. The line above the boxes changes to "Credentials
   saved". The secret and token are never shown again.
5. Set **Folder** to a name such as `AP_CameraGimbal`. The camera creates it. For
   Dropbox it is inside your app's folder.
6. Choose **What to upload** and **After uploading**. Leave **After uploading** on
   *keep* the first time, so nothing on the camera is deleted until you have
   checked the files arrived.
7. Set **Upload to cloud** to **Enabled**.
8. **Click Save settings** at the bottom of the page. This is a different button
   from **Save credentials**. Save credentials stores only the key. Cloud service,
   Folder and Upload to cloud are saved by **Save settings**.
9. Click **Upload now**.

**Messages to your ground station.** The camera also sends short text messages to
the GCS, at INFO severity: **Upload to Dropbox started (N files)** (or "Google Drive") when there are files
to upload, **Upload to Dropbox complete** when they are all done, and **Upload to
Dropbox failed** if it cannot upload (it keeps retrying, and says "complete" when it
succeeds). It
says nothing when there is nothing new to upload.

**It remembers what it has uploaded.** Once a file is uploaded and checked, the camera
remembers it (by name, size and modification time), so the next check looks only at new
or changed files and finishes at once, with no network use.

At the top of the tab the camera shows which cloud service it is using
("Cloud service in use: …") and what it is doing. It uploads while the camera is
recording, but not while the vehicle is **armed**. The file being recorded is
sent after the recording ends. **Upload now** starts at once, even while armed
or recording. It also waits until the camera's clock has been set.

**Where the files go**

- Google Drive: the folder you named, in **My Drive**, with `photos` and
  `recordings` inside it.
- Dropbox: **Apps**, then your app's name, then the folder you named, with `photos`
  and `recordings` inside it. The `Apps` folder is at the top level of your
  Dropbox.

---

## If something goes wrong

The camera shows the reason after **Last error** on the Cloud Upload tab.

| What you see | Why, and what to do |
|---|---|
| *Google refused the credentials: the token has expired or been revoked* | The token is no longer valid. Most often the consent screen is still in **Testing** (see Google step 4). Publish the app, then run `rclone authorize` again and save the new token. It also happens if you removed access in your Google account. |
| `rclone authorize` says *The OAuth client was not found* (Google) | The client ID you typed is not the one Google has. Copy the ID again from the Credentials page. Check that you replaced `CLIENT_ID` and did not type the word. |
| `rclone authorize` says *The provided client secret is invalid* | The secret is wrong or you typed `CLIENT_SECRET` literally. Copy it again, or add a new secret to the client. |
| *The Dropbox app does not have the … permission* | Tick the named permission on the app's **Permissions** tab, click **Submit**, then run `rclone authorize` again and save the new token. |
| `rclone authorize` fails for Dropbox with a redirect error | Add `http://localhost:53682/` under **Redirect URIs** on the app's Settings tab. |
| Dropbox says the token is not valid or has no refresh token | You may have used **Generate access token**. Use `rclone authorize "dropbox"` instead. |
| The Cloud Upload page says the credentials were not saved | The token must be one line starting with `{` and containing both `access_token` and `refresh_token`. Copy the whole line from rclone. |
| I saved the credentials but nothing uploads | **Save credentials** is not enough. Set **Cloud service** and **Upload to cloud** and click **Save settings**. Check that "Cloud service in use" names the service you want. |
| Status says *Waiting for the clock to be set* | The camera's date and time are not set yet. They come from the flight controller or the network. |
| Status says uploads are *disabled* with a reason | The reason says what is missing: the folder name, the credentials or the CA certificates file. |
| I cannot find the files in Dropbox | Look in **Apps**, then your app's name. They are not in the top level of your Dropbox. |
| I deleted the files in my Drive or Dropbox and they do not come back | The camera remembers what it uploaded and only sends new or changed files. To send everything again, change the **Folder** name (a new folder starts afresh), or delete the file `/etc/ap_cameragimbal/upload-cache-<service>.txt` on the camera. |
| Nothing uploads while the vehicle is armed | That is by design. Uploads wait until the vehicle has disarmed. **Upload now** works even while armed. |
| The recording in progress is not in the cloud | The file being recorded is sent when the recording ends. |
| The status says *N file(s) could not be uploaded* | A file that cannot be read, or whose name or path the service cannot take, is skipped, and the others are still sent. The camera's log says which file. |
| The status says *N unfinished file(s) (\*.part, \*.tmp) left in the folders* | Files being written are not sent. Ones that have not changed for ten minutes were left behind by a crash or a power cut. They are not sent and not deleted; delete them yourself (the camera's log says where they are). |

## Removing access

- On the camera: Cloud Upload tab, **Remove credentials** for that service. This
  deletes the saved key from the camera.
- Google: <https://myaccount.google.com/permissions>, choose your app, **Remove
  access**.
- Dropbox: <https://www.dropbox.com/account/connected_apps>, choose your app,
  **Disconnect**.

## How safe is this?

- The camera can reach only what you allowed. With Dropbox that is one app
  folder. With Google Drive, the token rclone creates covers your Drive, so use a
  Google account just for the camera if that matters to you.
- The client secret and token are stored on the camera in a file only the camera
  software can read. The web page never shows them again. On the ZR10 that file
  is on the SD card with the other settings, and a card has no file permissions,
  so anyone who holds the card can read it: keep the card safe, and if it is
  lost, revoke the app's access in your Google or Dropbox account.
- With **After uploading** set to *delete*, a photo or recording is deleted from
  the camera only after the service reports the same size and checksum as the
  local file.
- A file with the same name as one already in the cloud folder, but different
  contents, replaces it. File names come from the camera's date and time, so two
  cameras sharing one folder, or a camera whose clock was not set when it took a
  picture, can produce the same name. Give each camera its own **Folder**, and
  use *keep* rather than *delete* if that worries you.
