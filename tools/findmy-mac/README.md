# findmy-mac: Apple Find My reports through your own Mac

Since 12 October 2026, Apple refuses Find My logins from the Android anisette
that macless-haystack uses (error `-80045`). It only gives the report token
(`searchPartyToken`) to a macOS or iOS identity. A real Mac has one. These
tools let your Mac do the Apple side, and a small server show the result on
the findmy page.

```
 Mac (launchd, every 15 min)                     server (CT 110)                 iPhone / browser
 anisette_mac.py: AOSKit OTP headers
 findmy_mac.py fetch  -- encrypted reports -->  cache_server.py :6176  <-- POST --  macless-haystack page
   (Apple ID token, keys stay on the Mac)        behind Caddy + Authelia             decrypts with your key
```

## Steps (on the Mac)

1. Install the dependencies:
   `python3 -m pip install --user pyobjc-core pyobjc-framework-Cocoa requests srp pbkdf2 pycryptodome cryptography`
2. Run the probe: `python3 anisette_mac.py`. It should end with `OK: this Mac can make anisette headers`.
3. Clone macless-haystack. The login reuses its code:
   `git clone https://github.com/dchristl/macless-haystack.git ~/macless-haystack`
4. Log in once: `python3 findmy_mac.py login`. Give your Apple ID, password and the SMS code.
5. Fetch once by hand: `python3 findmy_mac.py fetch`. It pushes over
   `ssh root@pve2`; the key must work without a password prompt.
6. Fetch every 15 minutes: `python3 findmy_mac.py install`.

The keys come from `~/.kodefind/haystack/*.keys` (`tools/haystack/genkey.py`).
The token is kept in `~/.kodefind/findmy-mac/auth.json` (mode 600).

## Limits

* The page only updates while the Mac is awake and online.
* Apple can change this again at any time.
