#include "squirt.h"

#include <SD_MMC.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_random.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <cstring>

#include "player.h"

// ===========================================================================
// What this protects against, and what it doesn't
//
// The radio is open to anyone in range, so every byte arriving here is
// untrusted input from an unauthenticated stranger until proven otherwise.
// The design follows from that:
//
//   * Nothing happens without both users. A transfer needs a tap on the
//     sending device and a tap on the receiving one, and in between, both
//     users have to agree that the same six digits appear on both screens.
//     There is no silent-accept path, so no one can push a file onto a
//     device they aren't standing next to with its owner's cooperation.
//
//   * Those six digits are a short authentication string, the same trick
//     Bluetooth numeric comparison uses. Both ends do an ECDH exchange and
//     derive the code from the *shared secret*. An attacker relaying between
//     two devices has to substitute their own key on each side, which gives
//     them two different secrets, neither of which matches the other end's
//     code. So a mismatch is exactly what a man-in-the-middle looks like,
//     and matching digits mean the two screens really are talking to each
//     other. This is why the code must be compared out loud rather than
//     sent over the air -- it is worth nothing if the radio carries it.
//
//   * Everything after the exchange is AES-256-GCM with a key derived from
//     that secret, so content is confidential and tampering is detected.
//     Each direction gets its own key, every frame gets a fresh nonce from a
//     counter that only ever goes up, the plaintext header is authenticated
//     as additional data, and frames with a stale sequence number are
//     dropped -- so frames can't be replayed, reordered or edited in flight.
//
//   * A frame's tag is verified before its contents are looked at, let alone
//     acted on. Unauthenticated frames get parsed only far enough to find
//     the session they claim to belong to.
//
//   * Received files are treated as hostile. The sender's filename is not
//     used as given: it is reduced to a basename, filtered to a conservative
//     character set, rejected if it is hidden or has no playable extension,
//     and never allowed to overwrite an existing file. Content lands in a
//     temporary file, is checked against a SHA-256 the sender commits to,
//     and is only given its real name if it matches.
//
//   * Received files are never played automatically. This one is deliberate
//     and worth keeping: handing attacker-chosen bytes straight to an audio
//     decoder is how you turn a file transfer into code execution, and the
//     decoders here have a track record (see the FLAC bit-reader bugs in the
//     README). A human choosing to play a file they just accepted is a much
//     better gate than a device doing it on arrival.
//
//   * The radio is off unless the user opens the squirt screen, one transfer
//     runs at a time, sizes and counts are bounded, every state times out,
//     and malformed frames are dropped without reply. An attacker in range
//     can waste a few seconds of airtime; they cannot wedge the device or
//     make it allocate on their behalf.
//
// What it does not do: it does not hide *that* a transfer is happening, or
// the device names in beacons -- those are plaintext by necessity, since
// they're how devices find each other before any key exists. It also trusts
// the user to actually compare the digits rather than tapping through. And
// pairings are per-transfer by design: nothing is remembered, so there is no
// stored key material to steal, at the cost of confirming every time.
// ===========================================================================

namespace {

// --- wire protocol ---------------------------------------------------------

constexpr uint8_t kMagic0       = 'S';
constexpr uint8_t kMagic1       = 'Q';
constexpr uint8_t kProtoVersion = 1;

// ESP-NOW is fixed at 250 bytes of payload, which everything below is sized
// against. The channel has to match on both devices; since they run the same
// firmware, a constant is enough.
constexpr size_t  kMaxFrame = 250;
constexpr uint8_t kChannel  = 1;

enum : uint8_t {
    MSG_BEACON    = 1,    // broadcast, plaintext: "I am here, this is my name"
    MSG_OFFER     = 2,    // plaintext: sender's public key + nonce
    MSG_OFFER_ACK = 3,    // plaintext: receiver's public key + nonce
    MSG_CONFIRM   = 4,    // sealed: sender confirmed the code; carries file metadata
    MSG_ACCEPT    = 5,    // sealed: receiving user accepted
    MSG_DECLINE   = 6,    // sealed: receiving user said no
    MSG_DATA      = 7,    // sealed: one chunk
    MSG_ACK       = 8,    // sealed: next chunk index the receiver wants
    MSG_FIN       = 9,    // sealed: no more data, here is the whole-file hash
    MSG_RESULT    = 10,   // sealed: verified / didn't verify
    MSG_ABORT     = 11,   // plaintext, best effort: I'm giving up
};

struct __attribute__((packed)) Hdr {
    uint8_t  magic0;
    uint8_t  magic1;
    uint8_t  version;
    uint8_t  type;
    uint32_t session;
    uint64_t seq;      // per-direction, strictly increasing, also the GCM nonce
};
static_assert(sizeof(Hdr) == 16, "header must be packed to 16 bytes");

constexpr size_t kTagBytes = 16;
constexpr size_t kMaxSealed = kMaxFrame - sizeof(Hdr) - kTagBytes;   // 218

// Public keys go over the wire in whatever form mbedtls' ECDH writes, which
// is fine because both ends are the same build. 66 bytes for P-256; the
// slack is so a curve change doesn't silently truncate.
constexpr size_t kMaxPubKey = 80;

struct __attribute__((packed)) KeyExchange {
    uint8_t nonce[16];
    char    name[24];
    uint8_t pubLen;
    uint8_t pub[kMaxPubKey];
};

struct __attribute__((packed)) Meta {
    char     filename[64];
    uint32_t bytes;
};

constexpr size_t kChunkBytes = 192;

struct __attribute__((packed)) DataChunk {
    uint32_t index;
    uint16_t len;
    uint8_t  data[kChunkBytes];
};
static_assert(sizeof(DataChunk) <= kMaxSealed, "data chunk must fit one frame");

struct __attribute__((packed)) AckMsg  { uint32_t nextWanted; };
struct __attribute__((packed)) FinMsg  { uint8_t sha256[32]; };
struct __attribute__((packed)) Result  { uint8_t ok; char message[48]; };

// --- limits ----------------------------------------------------------------
// Every one of these exists so a peer can't make this device do unbounded
// work, hold unbounded state, or wait forever.

constexpr uint32_t kMaxFileBytes    = 32u * 1024 * 1024;
constexpr uint32_t kBeaconEveryMs   = 1500;
constexpr uint32_t kPeerStaleMs     = 6000;
constexpr size_t   kMaxPeers        = 8;
constexpr uint32_t kHandshakeTimeoutMs = 15000;   // waiting on the other device
constexpr uint32_t kUserTimeoutMs      = 90000;   // waiting on a human
constexpr uint32_t kTransferStallMs    = 12000;   // no progress at all
constexpr uint32_t kRetryAfterMs       = 400;     // no ack -> resend the window
constexpr uint32_t kCooldownMs         = 2000;    // between transfers
constexpr uint32_t kWindowChunks       = 8;
constexpr uint32_t kMaxWindowRetries   = 24;

// The close-out handshake needs the same treatment as the data window. FIN and
// RESULT are single frames on a lossy radio, and losing either one used to end
// with the receiver holding a complete file it had never been told to keep.
constexpr uint32_t kFinRetryMs         = 600;
constexpr uint32_t kMaxFinRetries      = 20;      // ~12s of asking
constexpr uint32_t kLingerMs           = 5000;    // keep answering repeat FINs

const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// --- state -----------------------------------------------------------------

struct RxFrame {
    uint8_t mac[6];
    uint8_t len;
    uint8_t data[kMaxFrame];
};

struct UiCmd {
    enum Type : uint8_t { SendTo, ConfirmCode, RespondOffer, Cancel } type;
    uint8_t mac[6];
    bool    flag;
    char    path[160];
};

struct Session {
    bool     active   = false;
    bool     isSender = false;
    uint8_t  peer[6]  = {0};
    char     peerName[24] = {0};
    uint32_t id = 0;

    uint8_t  txKey[32] = {0};
    uint8_t  rxKey[32] = {0};
    uint64_t txSeq     = 0;
    uint64_t rxSeqSeen = 0;
    uint32_t code      = 0;
    bool     keysReady = false;
    bool     localOk   = false;   // this user confirmed the digits
    bool     remoteOk  = false;   // a frame from them verified, so they hold the key

    char     filename[64] = {0};
    uint32_t totalBytes   = 0;
    uint32_t doneBytes    = 0;
    uint32_t chunkCount   = 0;

    // sender
    uint32_t base       = 0;      // lowest chunk not yet acked
    uint32_t nextToSend = 0;
    uint32_t hashedUpTo = 0;      // chunks folded into the digest, in order, once each
    uint32_t retries    = 0;
    uint32_t lastSendAt = 0;

    // sender: close-out
    uint8_t  finHash[32] = {0};   // kept so FIN can be sent again unchanged
    bool     finSent    = false;
    uint32_t finSentAt  = 0;
    uint32_t finRetries = 0;

    // receiver
    uint32_t nextWanted = 0;
    uint32_t sinceAck   = 0;

    // receiver: close-out. Saved and finished, but deliberately still alive so
    // a FIN the sender had to repeat gets the same answer instead of silence.
    bool     saved      = false;
    uint32_t savedAt    = 0;
    Result   savedReply = {};

    File     file;
    bool     fileOpen = false;
    mbedtls_sha256_context hash;
    bool     hashInit = false;

    uint32_t lastActivity = 0;
};

SemaphoreHandle_t gLock     = nullptr;   // guards gPeers, gStatus, gOffer, gCode
QueueHandle_t     gRxQueue  = nullptr;
QueueHandle_t     gUiQueue  = nullptr;
TaskHandle_t      gTask     = nullptr;

bool              gEnabled  = false;
bool              gStarted  = false;
char              gName[24] = {0};
uint32_t          gCooldownUntil = 0;

squirt::Peer      gPeers[kMaxPeers];
size_t            gPeerCount = 0;
squirt::Status    gStatus;
squirt::Offer     gOffer;
bool              gOfferPending = false;

Session           gSess;

// Owned by the task alone.
mbedtls_ecdh_context gEcdh;
bool                 gEcdhReady = false;
uint8_t              gMyNonce[16];
uint8_t              gMyPub[kMaxPubKey];
size_t               gMyPubLen = 0;
uint8_t              gPeerPub[kMaxPubKey];
size_t               gPeerPubLen = 0;
uint8_t              gPeerNonce[16];

uint8_t              gStage[4096];       // sender: chunks served from here
uint32_t             gStageBase = 0;     // chunk index gStage[0] belongs to
uint32_t             gStageLen  = 0;
bool                 gStageValid = false;

uint8_t              gWriteBuf[4096];    // receiver: batched card writes
uint32_t             gWriteLen = 0;

// --- small helpers ---------------------------------------------------------

void lock()   { if (gLock) xSemaphoreTake(gLock, portMAX_DELAY); }
void unlock() { if (gLock) xSemaphoreGive(gLock); }

void setStatus(squirt::State s, const char *msg) {
    lock();
    gStatus.state       = s;
    gStatus.transferred = gSess.doneBytes;
    gStatus.total       = gSess.totalBytes;
    strncpy(gStatus.peerName, gSess.peerName, sizeof(gStatus.peerName) - 1);
    gStatus.peerName[sizeof(gStatus.peerName) - 1] = '\0';
    if (msg) {
        strncpy(gStatus.message, msg, sizeof(gStatus.message) - 1);
        gStatus.message[sizeof(gStatus.message) - 1] = '\0';
    }
    unlock();
}

void setProgress() {
    lock();
    gStatus.transferred = gSess.doneBytes;
    gStatus.total       = gSess.totalBytes;
    unlock();
}

int rngCb(void *, unsigned char *out, size_t len) {
    // The hardware RNG is only properly seeded with the radio running, which
    // is guaranteed here: keys are never generated while squirt is disabled.
    esp_fill_random(out, len);
    return 0;
}

void sha256(const uint8_t *in, size_t len, uint8_t out[32]) {
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts_ret(&c, 0);
    mbedtls_sha256_update_ret(&c, in, len);
    mbedtls_sha256_finish_ret(&c, out);
    mbedtls_sha256_free(&c);
}

bool macEq(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 6) == 0; }

// HKDF (RFC 5869) over HMAC-SHA256. Written out here rather than calling
// mbedtls_hkdf() because MBEDTLS_HKDF_C isn't enabled in the prebuilt mbedtls
// the Arduino core ships -- the header is present but the symbol isn't, so it
// links only if you build your own IDF. mbedtls_md_hmac() is always there.
bool hkdfSha256(const uint8_t *salt, size_t saltLen,
                const uint8_t *ikm, size_t ikmLen,
                const char *info, size_t infoLen,
                uint8_t *out, size_t outLen) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || infoLen > 64 || outLen > 255 * 32) return false;

    uint8_t prk[32];
    if (mbedtls_md_hmac(md, salt, saltLen, ikm, ikmLen, prk) != 0) return false;

    uint8_t t[32];
    size_t  tLen = 0, done = 0;
    bool    ok   = true;
    for (uint8_t counter = 1; done < outLen && ok; counter++) {
        uint8_t block[32 + 64 + 1];
        size_t  n = 0;
        if (tLen) { memcpy(block, t, tLen); n = tLen; }
        memcpy(block + n, info, infoLen);
        n += infoLen;
        block[n++] = counter;

        ok = mbedtls_md_hmac(md, prk, sizeof(prk), block, n, t) == 0;
        if (!ok) break;
        tLen = sizeof(t);

        size_t take = (outLen - done) < tLen ? (outLen - done) : tLen;
        memcpy(out + done, t, take);
        done += take;
    }
    memset(prk, 0, sizeof(prk));
    memset(t, 0, sizeof(t));
    return ok;
}

// --- ESP-NOW plumbing ------------------------------------------------------

bool ensurePeer(const uint8_t mac[6]) {
    if (esp_now_is_peer_exist(mac)) return true;
    esp_now_peer_info_t p = {};
    memcpy(p.peer_addr, mac, 6);
    p.channel = kChannel;
    p.ifidx   = WIFI_IF_STA;
    p.encrypt = false;   // confidentiality is ours, not the driver's
    return esp_now_add_peer(&p) == ESP_OK;
}

bool sendRaw(const uint8_t mac[6], const uint8_t *data, size_t len) {
    if (len > kMaxFrame || !ensurePeer(mac)) return false;
    // The driver's transmit queue is short; a full queue is normal under load
    // and just means "try again in a moment", not a failure.
    for (int attempt = 0; attempt < 6; attempt++) {
        esp_err_t err = esp_now_send(mac, data, len);
        if (err == ESP_OK) return true;
        if (err != ESP_ERR_ESPNOW_NO_MEM) return false;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

bool sendPlain(const uint8_t mac[6], uint8_t type, uint32_t session,
               const void *payload, size_t len) {
    uint8_t frame[kMaxFrame];
    if (sizeof(Hdr) + len > kMaxFrame) return false;
    Hdr h = {kMagic0, kMagic1, kProtoVersion, type, session, 0};
    memcpy(frame, &h, sizeof(h));
    if (len) memcpy(frame + sizeof(h), payload, len);
    return sendRaw(mac, frame, sizeof(h) + len);
}

// Encrypt-and-authenticate one frame for the active session. The header is
// authenticated but not encrypted, so a tampered type or sequence number
// fails the tag rather than being silently accepted.
bool sendSealed(uint8_t type, const void *payload, size_t len) {
    if (!gSess.active || !gSess.keysReady || len > kMaxSealed) return false;

    uint8_t frame[kMaxFrame];
    Hdr h = {kMagic0, kMagic1, kProtoVersion, type, gSess.id, ++gSess.txSeq};
    memcpy(frame, &h, sizeof(h));

    uint8_t iv[12] = {0};
    memcpy(iv, &h.seq, 8);
    iv[8] = gSess.isSender ? 1 : 2;   // belt and braces; the keys already differ

    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    bool ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, gSess.txKey, 256) == 0 &&
              mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, len, iv, sizeof(iv),
                                        (const uint8_t *)&h, sizeof(h),
                                        payload ? (const uint8_t *)payload : frame,
                                        frame + sizeof(h),
                                        kTagBytes, frame + sizeof(h) + len) == 0;
    mbedtls_gcm_free(&g);
    if (!ok) return false;
    return sendRaw(gSess.peer, frame, sizeof(h) + len + kTagBytes);
}

// Verifies and decrypts. Returns false without touching `out` on any problem,
// so callers can only ever see authenticated plaintext.
bool openSealed(const Hdr &h, const uint8_t *body, size_t bodyLen,
                uint8_t *out, size_t outCap, size_t *outLen) {
    if (bodyLen < kTagBytes) return false;
    size_t len = bodyLen - kTagBytes;
    if (len > outCap) return false;

    uint8_t iv[12] = {0};
    memcpy(iv, &h.seq, 8);
    iv[8] = gSess.isSender ? 2 : 1;   // the other direction's tag

    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    bool ok = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, gSess.rxKey, 256) == 0 &&
              mbedtls_gcm_auth_decrypt(&g, len, iv, sizeof(iv),
                                       (const uint8_t *)&h, sizeof(h),
                                       body + len, kTagBytes, body, out) == 0;
    mbedtls_gcm_free(&g);
    if (!ok) return false;
    *outLen = len;
    return true;
}

void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
    // WiFi task context: copy and get out. No parsing, no card, no crypto.
    if (!mac || !data || len <= 0 || (size_t)len > kMaxFrame) return;
    RxFrame f;
    memcpy(f.mac, mac, 6);
    f.len = (uint8_t)len;
    memcpy(f.data, data, len);
    // A full queue means someone is flooding us; dropping is the right answer.
    xQueueSend(gRxQueue, &f, 0);
}

// --- peer table ------------------------------------------------------------

void notePeer(const uint8_t mac[6], const char *name) {
    lock();
    for (size_t i = 0; i < gPeerCount; i++) {
        if (macEq(gPeers[i].mac, mac)) {
            strncpy(gPeers[i].name, name, sizeof(gPeers[i].name) - 1);
            gPeers[i].name[sizeof(gPeers[i].name) - 1] = '\0';
            gPeers[i].lastSeen = millis();
            unlock();
            return;
        }
    }
    if (gPeerCount < kMaxPeers) {
        squirt::Peer &p = gPeers[gPeerCount++];
        memcpy(p.mac, mac, 6);
        strncpy(p.name, name, sizeof(p.name) - 1);
        p.name[sizeof(p.name) - 1] = '\0';
        p.lastSeen = millis();
    }
    // Table full: ignore. Better to keep showing devices the user has seen
    // than to let a stranger churn them out of the list.
    unlock();
}

void expirePeers() {
    lock();
    uint32_t now = millis();
    size_t   w   = 0;
    for (size_t i = 0; i < gPeerCount; i++) {
        if (now - gPeers[i].lastSeen < kPeerStaleMs) gPeers[w++] = gPeers[i];
    }
    gPeerCount = w;
    unlock();
}

// --- filenames -------------------------------------------------------------

// Turns whatever the sender claims into something safe to create on the card,
// or rejects it. Never repairs a path into a path: anything with a separator
// is treated as traversal and reduced to its last component.
bool sanitizeFilename(const char *in, char *out, size_t outSize) {
    if (!in || !out || outSize < 2) return false;

    const char *base = in;
    for (const char *p = in; *p && (size_t)(p - in) < 200; p++) {
        if (*p == '/' || *p == '\\' || *p == ':') base = p + 1;
    }

    size_t n = 0;
    bool   substantive = false;
    for (const char *p = base; *p && n + 1 < outSize; p++) {
        unsigned char c = (unsigned char)*p;
        bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '_' ||
                       c == '-' || c == '(' || c == ')' || c == '\'' || c == '&' ||
                       c == ',' || c == '!' || c == '+' || c == '#';
        out[n++] = allowed ? (char)c : '_';
        if (allowed && c != ' ' && c != '.') substantive = true;
    }
    out[n] = '\0';

    if (!substantive) return false;                  // dots and spaces only
    if (out[0] == '.') return false;                 // hidden, or "." / ".."
    if (strstr(out, "..") != nullptr) return false;  // after the basename strip too
    return player::isPlayable(std::string(out));     // extension whitelist
}

// Card lock must already be held.
bool uniqueTarget(const char *name, char *out, size_t outSize) {
    snprintf(out, outSize, "%s/%s", squirt::inboxPath(), name);
    if (!SD_MMC.exists(out)) return true;

    std::string n(name);
    size_t      dot  = n.find_last_of('.');
    std::string stem = dot == std::string::npos ? n : n.substr(0, dot);
    std::string ext  = dot == std::string::npos ? "" : n.substr(dot);

    for (int i = 2; i < 100; i++) {
        snprintf(out, outSize, "%s/%s (%d)%s", squirt::inboxPath(), stem.c_str(), i, ext.c_str());
        if (!SD_MMC.exists(out)) return true;
    }
    return false;
}

const char *tempPath() { return "/Squirt/_incoming.tmp"; }

// --- session lifecycle -----------------------------------------------------

void closeSessionFile(bool removeTemp) {
    if (gSess.fileOpen) {
        gSess.file.close();
        gSess.fileOpen = false;
    }
    if (removeTemp && SD_MMC.exists(tempPath())) SD_MMC.remove(tempPath());
}

void resetSession(squirt::State s, const char *msg) {
    if (gSess.active && player::lockCard(2000)) {
        closeSessionFile(!gSess.isSender);
        player::unlockCard();
    } else if (gSess.fileOpen) {
        // Couldn't get the card; drop the handle anyway rather than leak it.
        gSess.file.close();
        gSess.fileOpen = false;
    }
    if (gSess.hashInit) {
        mbedtls_sha256_free(&gSess.hash);
        gSess.hashInit = false;
    }
    if (gEcdhReady) {
        mbedtls_ecdh_free(&gEcdh);
        gEcdhReady = false;
    }
    if (gSess.active && !macEq(gSess.peer, kBroadcast)) esp_now_del_peer(gSess.peer);

    // Wipe key material rather than leaving it in RAM for the next session.
    memset(gSess.txKey, 0, sizeof(gSess.txKey));
    memset(gSess.rxKey, 0, sizeof(gSess.rxKey));

    gSess = Session{};
    gStageValid = false;
    gWriteLen   = 0;

    lock();
    gOfferPending = false;
    unlock();

    gCooldownUntil = millis() + kCooldownMs;
    setStatus(s, msg);
}

void abortSession(const char *why) {
    if (gSess.active) sendPlain(gSess.peer, MSG_ABORT, gSess.id, nullptr, 0);
    Serial.printf("[squirt] aborting: %s\n", why);
    resetSession(squirt::State::Failed, why);
}

// Derives both directional keys and the six digits the users compare. The
// transcript binds the code to both identities and both fresh nonces, so a
// recorded exchange can't be replayed into a new session.
bool deriveKeys(bool weAreSender) {
    uint8_t secret[32];
    size_t  secretLen = 0;
    if (mbedtls_ecdh_calc_secret(&gEcdh, &secretLen, secret, sizeof(secret), rngCb, nullptr) != 0) {
        return false;
    }

    uint8_t myMac[6], peerMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, myMac);
    memcpy(peerMac, gSess.peer, 6);

    // Canonical order: the sender's material always comes first, so both
    // devices hash exactly the same bytes.
    uint8_t transcript[2 * 6 + 2 * kMaxPubKey + 2 * 16 + 16];
    size_t  t = 0;
    auto    put = [&](const void *p, size_t n) {
        if (t + n <= sizeof(transcript)) { memcpy(transcript + t, p, n); t += n; }
    };
    put("ES3C28P-squirt-1", 16);
    if (weAreSender) {
        put(myMac, 6);      put(peerMac, 6);
        put(gMyPub, gMyPubLen);   put(gPeerPub, gPeerPubLen);
        put(gMyNonce, 16);        put(gPeerNonce, 16);
    } else {
        put(peerMac, 6);    put(myMac, 6);
        put(gPeerPub, gPeerPubLen); put(gMyPub, gMyPubLen);
        put(gPeerNonce, 16);        put(gMyNonce, 16);
    }

    uint8_t salt[32];
    sha256(transcript, t, salt);

    uint8_t okm[64];
    if (!hkdfSha256(salt, sizeof(salt), secret, secretLen,
                    "squirt keys v1", 14, okm, sizeof(okm))) {
        memset(secret, 0, sizeof(secret));
        return false;
    }

    // okm[0..31] is always sender->receiver, so each side picks the right half.
    memcpy(gSess.txKey, weAreSender ? okm : okm + 32, 32);
    memcpy(gSess.rxKey, weAreSender ? okm + 32 : okm, 32);

    uint8_t sas[4];
    bool ok = hkdfSha256(salt, sizeof(salt), secret, secretLen,
                         "squirt sas v1", 13, sas, sizeof(sas));
    memset(secret, 0, sizeof(secret));
    memset(okm, 0, sizeof(okm));
    if (!ok) return false;

    uint32_t n = ((uint32_t)sas[0] << 24) | ((uint32_t)sas[1] << 16) |
                 ((uint32_t)sas[2] << 8) | sas[3];
    gSess.code      = n % 1000000u;
    gSess.keysReady = true;
    return true;
}

bool makeKeypair() {
    if (gEcdhReady) mbedtls_ecdh_free(&gEcdh);
    mbedtls_ecdh_init(&gEcdh);
    gEcdhReady = true;
    if (mbedtls_ecdh_setup(&gEcdh, MBEDTLS_ECP_DP_SECP256R1) != 0) return false;
    gMyPubLen = 0;
    if (mbedtls_ecdh_make_public(&gEcdh, &gMyPubLen, gMyPub, sizeof(gMyPub), rngCb, nullptr) != 0) {
        return false;
    }
    esp_fill_random(gMyNonce, sizeof(gMyNonce));
    return true;
}

// --- sender ----------------------------------------------------------------

bool readChunk(uint32_t index, uint8_t *out, uint16_t *outLen) {
    uint32_t offset = index * kChunkBytes;
    if (offset >= gSess.totalBytes) return false;

    bool inStage = gStageValid && index >= gStageBase &&
                   (index - gStageBase) * kChunkBytes < gStageLen;
    if (!inStage) {
        if (!player::lockCard(1500)) return false;
        bool ok = gSess.fileOpen && gSess.file.seek(offset);
        int  n  = ok ? gSess.file.read(gStage, sizeof(gStage)) : -1;
        player::unlockCard();
        if (n <= 0) return false;
        gStageBase  = index;
        gStageLen   = (uint32_t)n;
        gStageValid = true;
    }

    uint32_t within = (index - gStageBase) * kChunkBytes;
    if (within >= gStageLen) return false;
    uint32_t avail  = gStageLen - within;
    uint32_t remain = gSess.totalBytes - offset;
    uint32_t n      = kChunkBytes;
    if (n > avail)  n = avail;
    if (n > remain) n = remain;
    memcpy(out, gStage + within, n);
    *outLen = (uint16_t)n;
    return true;
}

void pumpSender() {
    uint32_t now = millis();

    while (gSess.nextToSend < gSess.chunkCount &&
           gSess.nextToSend < gSess.base + kWindowChunks) {
        DataChunk c;
        c.index = gSess.nextToSend;
        if (!readChunk(c.index, c.data, &c.len)) {
            abortSession("card read failed");
            return;
        }
        // Fold each chunk into the digest the first time it is read, never on a
        // retransmit -- the receiver hashes the byte stream once and in order,
        // so this has to match exactly.
        if (c.index == gSess.hashedUpTo && gSess.hashInit) {
            mbedtls_sha256_update_ret(&gSess.hash, c.data, c.len);
            gSess.hashedUpTo++;
        }
        if (!sendSealed(MSG_DATA, &c, sizeof(c.index) + sizeof(c.len) + c.len)) break;
        gSess.nextToSend++;
        gSess.lastSendAt = now;
    }

    // Nothing acknowledged for a while: rewind to the window base and resend.
    if (gSess.nextToSend > gSess.base && now - gSess.lastSendAt > kRetryAfterMs) {
        if (++gSess.retries > kMaxWindowRetries) {
            abortSession("peer stopped responding");
            return;
        }
        gSess.nextToSend = gSess.base;
        gSess.lastSendAt = now;
    }

    if (gSess.chunkCount && gSess.base >= gSess.chunkCount) {
        if (!gSess.finSent) {
            if (gSess.hashInit) {
                mbedtls_sha256_finish_ret(&gSess.hash, gSess.finHash);
                mbedtls_sha256_free(&gSess.hash);
                gSess.hashInit = false;
            }
            gSess.finSent   = true;
            gSess.finSentAt = now;
            sendSealed(MSG_FIN, gSess.finHash, sizeof(gSess.finHash));
            setStatus(squirt::State::Sending, "verifying");
        } else if (now - gSess.finSentAt > kFinRetryMs) {
            // FIN is one frame on the same lossy radio as everything else, and
            // the receiver has no other way to learn the data is complete. Sent
            // once and dropped, it would sit on a finished file waiting for a
            // message that never comes, then time out and throw the file away.
            // So keep asking until it answers, exactly like the data window.
            if (++gSess.finRetries > kMaxFinRetries) {
                abortSession("no confirmation from the other device");
                return;
            }
            gSess.finSentAt = now;
            sendSealed(MSG_FIN, gSess.finHash, sizeof(gSess.finHash));
        }
    }
}

bool beginSend(const uint8_t mac[6], const char *path) {
    if (!player::lockCard(2000)) {
        setStatus(squirt::State::Failed, "card busy");
        return false;
    }
    File f = SD_MMC.open(path, FILE_READ);
    if (!f || f.isDirectory()) {
        if (f) f.close();
        player::unlockCard();
        setStatus(squirt::State::Failed, "cannot open that track");
        return false;
    }
    uint32_t size = (uint32_t)f.size();
    if (size == 0 || size > kMaxFileBytes) {
        f.close();
        player::unlockCard();
        setStatus(squirt::State::Failed, size ? "file too large" : "file is empty");
        return false;
    }

    gSess = Session{};
    gSess.file     = f;
    gSess.fileOpen = true;
    player::unlockCard();

    gSess.active     = true;
    gSess.isSender   = true;
    memcpy(gSess.peer, mac, 6);
    gSess.id         = esp_random();
    gSess.totalBytes = size;
    gSess.chunkCount = (size + kChunkBytes - 1) / kChunkBytes;

    const char *slash = strrchr(path, '/');
    strncpy(gSess.filename, slash ? slash + 1 : path, sizeof(gSess.filename) - 1);
    gSess.filename[sizeof(gSess.filename) - 1] = '\0';

    mbedtls_sha256_init(&gSess.hash);
    mbedtls_sha256_starts_ret(&gSess.hash, 0);
    gSess.hashInit = true;

    lock();
    for (size_t i = 0; i < gPeerCount; i++) {
        if (macEq(gPeers[i].mac, mac)) {
            strncpy(gSess.peerName, gPeers[i].name, sizeof(gSess.peerName) - 1);
            break;
        }
    }
    unlock();

    if (!makeKeypair()) {
        resetSession(squirt::State::Failed, "key generation failed");
        return false;
    }

    KeyExchange kx = {};
    memcpy(kx.nonce, gMyNonce, sizeof(kx.nonce));
    strncpy(kx.name, gName, sizeof(kx.name) - 1);
    kx.pubLen = (uint8_t)gMyPubLen;
    memcpy(kx.pub, gMyPub, gMyPubLen);

    gSess.lastActivity = millis();
    if (!sendPlain(gSess.peer, MSG_OFFER, gSess.id, &kx, sizeof(kx))) {
        resetSession(squirt::State::Failed, "peer unreachable");
        return false;
    }
    setStatus(squirt::State::Handshaking, "connecting");
    return true;
}

// --- receiver --------------------------------------------------------------

bool flushWrite() {
    if (!gWriteLen) return true;
    if (!player::lockCard(2000)) return false;
    size_t n = gSess.fileOpen ? gSess.file.write(gWriteBuf, gWriteLen) : 0;
    player::unlockCard();
    if (n != gWriteLen) return false;
    gWriteLen = 0;
    return true;
}

void finishReceive(const FinMsg &fin) {
    if (!flushWrite()) { abortSession("card write failed"); return; }

    uint8_t got[32];
    mbedtls_sha256_finish_ret(&gSess.hash, got);
    mbedtls_sha256_free(&gSess.hash);
    gSess.hashInit = false;

    Result r = {};
    if (memcmp(got, fin.sha256, 32) != 0) {
        r.ok = 0;
        strncpy(r.message, "checksum mismatch", sizeof(r.message) - 1);
        sendSealed(MSG_RESULT, &r, sizeof(r));
        abortSession("checksum mismatch -- file discarded");
        return;
    }

    if (!player::lockCard(3000)) { abortSession("card busy"); return; }
    gSess.file.close();
    gSess.fileOpen = false;

    char target[192];
    bool named = uniqueTarget(gSess.filename, target, sizeof(target));
    bool moved = named && SD_MMC.rename(tempPath(), target);
    player::unlockCard();

    if (!moved) {
        r.ok = 0;
        strncpy(r.message, "could not save", sizeof(r.message) - 1);
        sendSealed(MSG_RESULT, &r, sizeof(r));
        abortSession("could not save the file");
        return;
    }

    r.ok = 1;
    strncpy(r.message, "saved", sizeof(r.message) - 1);
    sendSealed(MSG_RESULT, &r, sizeof(r));
    Serial.printf("[squirt] received %s (%u bytes)\n", target, (unsigned)gSess.totalBytes);

    // The file is safe on the card, so this side is done -- but don't tear the
    // session down yet. RESULT can be dropped just as easily as FIN was, and if
    // it is, the sender will ask again; without a session to answer with, it
    // would report a failure for a transfer that actually succeeded. Linger,
    // keep the answer, and let checkTimeouts() close this out.
    gSess.savedReply = r;
    gSess.saved      = true;
    gSess.savedAt    = millis();
    setStatus(squirt::State::Complete, "saved to /Squirt");
}

void handleData(const DataChunk &c, size_t payloadLen) {
    if (payloadLen < sizeof(c.index) + sizeof(c.len)) return;
    if (c.len > kChunkBytes || sizeof(c.index) + sizeof(c.len) + c.len > payloadLen) return;

    if (c.index != gSess.nextWanted) {
        // Out of order: say what we actually want so the sender rewinds now
        // rather than after a timeout.
        AckMsg a = {gSess.nextWanted};
        sendSealed(MSG_ACK, &a, sizeof(a));
        return;
    }
    // Refuse to write more than was promised, however many chunks arrive.
    if ((uint64_t)gSess.doneBytes + c.len > gSess.totalBytes) {
        abortSession("peer sent more than it promised");
        return;
    }

    if (gWriteLen + c.len > sizeof(gWriteBuf) && !flushWrite()) {
        abortSession("card write failed");
        return;
    }
    memcpy(gWriteBuf + gWriteLen, c.data, c.len);
    gWriteLen += c.len;

    mbedtls_sha256_update_ret(&gSess.hash, c.data, c.len);
    gSess.doneBytes += c.len;
    gSess.nextWanted++;

    if (++gSess.sinceAck >= 4 || gSess.doneBytes >= gSess.totalBytes) {
        gSess.sinceAck = 0;
        AckMsg a = {gSess.nextWanted};
        sendSealed(MSG_ACK, &a, sizeof(a));
    }
    setProgress();
}

bool beginReceiveFile() {
    if (!player::lockCard(2000)) return false;
    if (!SD_MMC.exists(squirt::inboxPath())) SD_MMC.mkdir(squirt::inboxPath());

    // Don't start something the card can't finish.
    uint64_t freeBytes = SD_MMC.totalBytes() - SD_MMC.usedBytes();
    if (freeBytes < (uint64_t)gSess.totalBytes + 512u * 1024u) {
        player::unlockCard();
        return false;
    }
    if (SD_MMC.exists(tempPath())) SD_MMC.remove(tempPath());
    gSess.file     = SD_MMC.open(tempPath(), FILE_WRITE);
    gSess.fileOpen = (bool)gSess.file;
    player::unlockCard();

    if (!gSess.fileOpen) return false;
    mbedtls_sha256_init(&gSess.hash);
    mbedtls_sha256_starts_ret(&gSess.hash, 0);
    gSess.hashInit = true;
    gWriteLen = 0;
    return true;
}

// --- frame dispatch --------------------------------------------------------

void handleOffer(const uint8_t *mac, const Hdr &h, const uint8_t *body, size_t bodyLen) {
    if (bodyLen < sizeof(KeyExchange)) return;
    const KeyExchange *kx = (const KeyExchange *)body;
    if (kx->pubLen == 0 || kx->pubLen > kMaxPubKey) return;

    if (gSess.active) {
        // Both users tapped send at the same moment. Rather than have both
        // transfers fail, settle it the same way on both devices: the lower
        // MAC address wins and keeps sending.
        uint8_t myMac[6];
        esp_wifi_get_mac(WIFI_IF_STA, myMac);
        bool theyWin = gSess.isSender && macEq(gSess.peer, mac) && memcmp(mac, myMac, 6) < 0;
        if (!theyWin) {
            sendPlain(mac, MSG_ABORT, h.session, nullptr, 0);
            return;
        }
        resetSession(squirt::State::Idle, "yielding to their offer");
        gCooldownUntil = 0;   // the cooldown resetSession() just armed is not for this
    }
    if (millis() < gCooldownUntil) return;

    char name[24];
    memcpy(name, kx->name, sizeof(name));
    name[sizeof(name) - 1] = '\0';

    gSess = Session{};
    gSess.active   = true;
    gSess.isSender = false;
    gSess.id       = h.session;
    memcpy(gSess.peer, mac, 6);
    strncpy(gSess.peerName, name, sizeof(gSess.peerName) - 1);

    gPeerPubLen = kx->pubLen;
    memcpy(gPeerPub, kx->pub, gPeerPubLen);
    memcpy(gPeerNonce, kx->nonce, sizeof(gPeerNonce));

    if (!makeKeypair() ||
        mbedtls_ecdh_read_public(&gEcdh, gPeerPub, gPeerPubLen) != 0 ||
        !deriveKeys(false)) {
        resetSession(squirt::State::Failed, "key agreement failed");
        return;
    }

    KeyExchange reply = {};
    memcpy(reply.nonce, gMyNonce, sizeof(reply.nonce));
    strncpy(reply.name, gName, sizeof(reply.name) - 1);
    reply.pubLen = (uint8_t)gMyPubLen;
    memcpy(reply.pub, gMyPub, gMyPubLen);

    gSess.lastActivity = millis();
    if (!sendPlain(gSess.peer, MSG_OFFER_ACK, gSess.id, &reply, sizeof(reply))) {
        resetSession(squirt::State::Failed, "peer unreachable");
        return;
    }
    setStatus(squirt::State::Verifying, "check the code");
}

void handleOfferAck(const uint8_t *mac, const Hdr &h, const uint8_t *body, size_t bodyLen) {
    if (!gSess.active || !gSess.isSender || gSess.keysReady) return;
    if (h.session != gSess.id || !macEq(mac, gSess.peer)) return;
    if (bodyLen < sizeof(KeyExchange)) return;

    const KeyExchange *kx = (const KeyExchange *)body;
    if (kx->pubLen == 0 || kx->pubLen > kMaxPubKey) return;

    gPeerPubLen = kx->pubLen;
    memcpy(gPeerPub, kx->pub, gPeerPubLen);
    memcpy(gPeerNonce, kx->nonce, sizeof(gPeerNonce));
    if (gSess.peerName[0] == '\0') {
        memcpy(gSess.peerName, kx->name, sizeof(gSess.peerName));
        gSess.peerName[sizeof(gSess.peerName) - 1] = '\0';
    }

    if (mbedtls_ecdh_read_public(&gEcdh, gPeerPub, gPeerPubLen) != 0 || !deriveKeys(true)) {
        abortSession("key agreement failed");
        return;
    }
    gSess.lastActivity = millis();
    setStatus(squirt::State::Verifying, "check the code");
}

void handleSealed(const Hdr &h, const uint8_t *body, size_t bodyLen) {
    uint8_t  plain[kMaxSealed];
    size_t   plainLen = 0;
    if (!openSealed(h, body, bodyLen, plain, sizeof(plain), &plainLen)) return;

    // Only now is any of this trustworthy.
    gSess.rxSeqSeen   = h.seq;
    gSess.remoteOk    = true;
    gSess.lastActivity = millis();

    switch (h.type) {
    case MSG_CONFIRM: {
        if (gSess.isSender || plainLen < sizeof(Meta)) return;
        const Meta *m = (const Meta *)plain;
        char claimed[64];
        memcpy(claimed, m->filename, sizeof(claimed));
        claimed[sizeof(claimed) - 1] = '\0';

        char safe[64];
        if (!sanitizeFilename(claimed, safe, sizeof(safe))) {
            Result r = {0, {0}};
            strncpy(r.message, "unacceptable filename", sizeof(r.message) - 1);
            sendSealed(MSG_RESULT, &r, sizeof(r));
            abortSession("rejected the sender's filename");
            return;
        }
        uint32_t size = m->bytes;
        if (size == 0 || size > kMaxFileBytes) {
            abortSession("offered file is an implausible size");
            return;
        }

        strncpy(gSess.filename, safe, sizeof(gSess.filename) - 1);
        gSess.totalBytes = size;
        gSess.chunkCount = (size + kChunkBytes - 1) / kChunkBytes;

        if (!gSess.localOk) return;   // this user hasn't confirmed the digits yet

        lock();
        strncpy(gOffer.from, gSess.peerName, sizeof(gOffer.from) - 1);
        strncpy(gOffer.filename, gSess.filename, sizeof(gOffer.filename) - 1);
        gOffer.bytes  = size;
        gOfferPending = true;
        unlock();
        setStatus(squirt::State::Deciding, "accept this track?");
        return;
    }

    case MSG_ACCEPT:
        if (!gSess.isSender) return;
        gStageValid      = false;
        gSess.base       = 0;
        gSess.nextToSend = 0;
        gSess.lastSendAt = millis();
        setStatus(squirt::State::Sending, "sending");
        return;

    case MSG_DECLINE:
        if (!gSess.isSender) return;
        resetSession(squirt::State::Failed, "they declined");
        return;

    case MSG_DATA:
        if (gSess.isSender) return;
        handleData(*(const DataChunk *)plain, plainLen);
        return;

    case MSG_ACK: {
        if (!gSess.isSender || plainLen < sizeof(AckMsg)) return;
        const AckMsg *a = (const AckMsg *)plain;
        if (a->nextWanted > gSess.chunkCount) return;
        if (a->nextWanted > gSess.base) {
            gSess.base    = a->nextWanted;
            gSess.retries = 0;
            uint64_t done = (uint64_t)gSess.base * kChunkBytes;
            gSess.doneBytes = done > gSess.totalBytes ? gSess.totalBytes : (uint32_t)done;
            setProgress();
        } else if (a->nextWanted < gSess.base) {
            gSess.nextToSend = a->nextWanted;   // shouldn't happen; resync anyway
        }
        return;
    }

    case MSG_FIN:
        if (gSess.isSender || plainLen < sizeof(FinMsg)) return;
        if (gSess.saved) {
            // Already written and renamed; the sender simply didn't hear the
            // answer. Repeat it verbatim rather than redoing any of the work.
            sendSealed(MSG_RESULT, &gSess.savedReply, sizeof(gSess.savedReply));
            return;
        }
        if (gSess.doneBytes != gSess.totalBytes) {
            abortSession("transfer ended early");
            return;
        }
        finishReceive(*(const FinMsg *)plain);
        return;

    case MSG_RESULT: {
        if (!gSess.isSender || plainLen < sizeof(Result)) return;
        const Result *r = (const Result *)plain;
        char msg[sizeof(r->message)];
        memcpy(msg, r->message, sizeof(msg));
        msg[sizeof(msg) - 1] = '\0';
        resetSession(r->ok ? squirt::State::Complete : squirt::State::Failed,
                     r->ok ? "sent" : msg);
        return;
    }

    default:
        return;
    }
}

void handleFrame(const RxFrame &f) {
    if (f.len < sizeof(Hdr)) return;
    Hdr h;
    memcpy(&h, f.data, sizeof(h));
    if (h.magic0 != kMagic0 || h.magic1 != kMagic1 || h.version != kProtoVersion) return;

    const uint8_t *body    = f.data + sizeof(Hdr);
    size_t         bodyLen = f.len - sizeof(Hdr);

    switch (h.type) {
    case MSG_BEACON: {
        if (bodyLen < 24) return;
        char name[24];
        memcpy(name, body, sizeof(name));
        name[sizeof(name) - 1] = '\0';
        notePeer(f.mac, name);
        return;
    }
    case MSG_OFFER:
        handleOffer(f.mac, h, body, bodyLen);
        return;
    case MSG_OFFER_ACK:
        handleOfferAck(f.mac, h, body, bodyLen);
        return;
    case MSG_ABORT:
        // Plaintext, so treat it as a hint, not an instruction: only act on it
        // for the session it names, from the device we're actually talking to.
        if (gSess.active && h.session == gSess.id && macEq(f.mac, gSess.peer)) {
            // If the file is already saved, a sender giving up says nothing
            // about this side -- it just never heard the answer. Don't turn a
            // finished transfer into a reported failure. (This frame is also
            // unauthenticated, so refusing to act on it here costs nothing.)
            if (gSess.saved) {
                resetSession(squirt::State::Complete, "saved to /Squirt");
            } else {
                resetSession(squirt::State::Failed, "the other device stopped");
            }
        }
        return;
    default:
        break;
    }

    // Everything else must belong to the live session and be authenticated.
    if (!gSess.active || !gSess.keysReady) return;
    if (h.session != gSess.id || !macEq(f.mac, gSess.peer)) return;
    if (h.seq <= gSess.rxSeqSeen) return;   // replay or reorder
    handleSealed(h, body, bodyLen);
}

// --- UI commands -----------------------------------------------------------

void handleUiCmd(const UiCmd &c) {
    switch (c.type) {
    case UiCmd::SendTo:
        if (!gEnabled || gSess.active || millis() < gCooldownUntil) return;
        beginSend(c.mac, c.path);
        return;

    case UiCmd::ConfirmCode:
        if (!gSess.active || !gSess.keysReady) return;
        if (!c.flag) { abortSession("code did not match"); return; }
        gSess.localOk      = true;
        gSess.lastActivity = millis();
        if (gSess.isSender) {
            Meta m = {};
            strncpy(m.filename, gSess.filename, sizeof(m.filename) - 1);
            m.bytes = gSess.totalBytes;
            if (!sendSealed(MSG_CONFIRM, &m, sizeof(m))) {
                abortSession("could not reach the other device");
                return;
            }
            setStatus(squirt::State::Offering, "waiting for them to accept");
        } else if (gSess.totalBytes) {
            // Their metadata already arrived, so the decision can be shown now.
            lock();
            strncpy(gOffer.from, gSess.peerName, sizeof(gOffer.from) - 1);
            strncpy(gOffer.filename, gSess.filename, sizeof(gOffer.filename) - 1);
            gOffer.bytes  = gSess.totalBytes;
            gOfferPending = true;
            unlock();
            setStatus(squirt::State::Deciding, "accept this track?");
        } else {
            setStatus(squirt::State::Handshaking, "waiting for them");
        }
        return;

    case UiCmd::RespondOffer: {
        if (!gSess.active || gSess.isSender || !gSess.keysReady) return;
        lock();
        gOfferPending = false;
        unlock();
        if (!c.flag) {
            sendSealed(MSG_DECLINE, nullptr, 0);
            resetSession(squirt::State::Idle, "declined");
            return;
        }
        if (!beginReceiveFile()) { abortSession("no room on the card"); return; }
        gSess.lastActivity = millis();
        if (!sendSealed(MSG_ACCEPT, nullptr, 0)) { abortSession("could not accept"); return; }
        setStatus(squirt::State::Receiving, "receiving");
        return;
    }

    case UiCmd::Cancel:
        if (gSess.active) abortSession("cancelled");
        else setStatus(gEnabled ? squirt::State::Idle : squirt::State::Off, "");
        return;
    }
}

// --- task ------------------------------------------------------------------

void checkTimeouts() {
    if (!gSess.active) return;
    uint32_t idle = millis() - gSess.lastActivity;

    // Receiver, finished and holding the session open only to re-answer a FIN
    // the sender had to repeat. Once the sender has clearly stopped asking,
    // there is nothing left to answer with.
    if (gSess.saved) {
        if (millis() - gSess.savedAt > kLingerMs) {
            resetSession(squirt::State::Complete, "saved to /Squirt");
        }
        return;
    }

    switch (gStatus.state) {
    case squirt::State::Verifying:
    case squirt::State::Deciding:
    case squirt::State::Offering:
        if (idle > kUserTimeoutMs) abortSession("timed out waiting for a decision");
        break;
    case squirt::State::Handshaking:
        if (idle > kHandshakeTimeoutMs) abortSession("the other device didn't answer");
        break;
    case squirt::State::Sending:
        // Once FIN is out, silence is expected: the receiver is flushing,
        // hashing and renaming on a card it shares with the decoder, and sends
        // nothing meanwhile. The retry cap in pumpSender() is the authority on
        // giving up there, so the stall timer must not fire underneath it.
        if (!gSess.finSent && idle > kTransferStallMs) abortSession("transfer stalled");
        break;
    case squirt::State::Receiving:
        if (idle > kTransferStallMs) abortSession("transfer stalled");
        break;
    default:
        break;
    }
}

void squirtTask(void *) {
    uint32_t lastBeacon = 0;
    for (;;) {
        if (!gEnabled) {
            // Still on air for a moment longer (setEnabled waits before taking
            // the radio down), so tell the peer rather than leaving it hanging.
            if (gSess.active) abortSession("squirt turned off");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        UiCmd cmd;
        while (xQueueReceive(gUiQueue, &cmd, 0) == pdTRUE) handleUiCmd(cmd);

        RxFrame f;
        while (xQueueReceive(gRxQueue, &f, 0) == pdTRUE) handleFrame(f);

        uint32_t now = millis();
        if (now - lastBeacon >= kBeaconEveryMs) {
            lastBeacon = now;
            char name[24] = {0};
            strncpy(name, gName, sizeof(name) - 1);
            sendPlain(kBroadcast, MSG_BEACON, 0, name, sizeof(name));
            expirePeers();
        }

        if (gSess.active && gSess.isSender && gStatus.state == squirt::State::Sending) {
            pumpSender();
        }
        checkTimeouts();

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void radioUp() {
    if (gStarted) return;
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    esp_wifi_set_ps(WIFI_PS_NONE);   // ESP-NOW has to hear frames at any moment
    esp_wifi_set_channel(kChannel, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[squirt] esp_now_init failed");
        WiFi.mode(WIFI_OFF);
        return;
    }
    esp_now_register_recv_cb(onRecv);
    ensurePeer(kBroadcast);

    if (gName[0] == '\0') {
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_STA, mac);
        snprintf(gName, sizeof(gName), "ES3C28P-%02X%02X", mac[4], mac[5]);
    }
    gStarted = true;
    Serial.printf("[squirt] on, calling myself %s\n", gName);
}

void radioDown() {
    if (!gStarted) return;
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    WiFi.mode(WIFI_OFF);
    gStarted = false;
    lock();
    gPeerCount = 0;
    unlock();
    Serial.println("[squirt] off");
}

void postUi(const UiCmd &c) {
    if (gUiQueue) xQueueSend(gUiQueue, &c, pdMS_TO_TICKS(20));
}

} // namespace

namespace squirt {

const char *inboxPath() { return "/Squirt"; }

bool begin(const char *deviceName) {
    if (gTask) return true;
    gLock    = xSemaphoreCreateMutex();
    gRxQueue = xQueueCreate(12, sizeof(RxFrame));
    gUiQueue = xQueueCreate(4, sizeof(UiCmd));
    if (!gLock || !gRxQueue || !gUiQueue) return false;

    if (deviceName && *deviceName) {
        strncpy(gName, deviceName, sizeof(gName) - 1);
        gName[sizeof(gName) - 1] = '\0';
    }
    gStatus.state = State::Off;

    // Below the audio task (5) on purpose: a squirt must never be the reason
    // playback skips. Pinned opposite it so the two don't fight for one core.
    BaseType_t ok = xTaskCreatePinnedToCore(squirtTask, "squirt", 8192, nullptr, 3, &gTask, 1);
    return ok == pdPASS;
}

void setEnabled(bool on) {
    if (on == gEnabled) return;
    gEnabled = on;
    if (on) {
        radioUp();
        if (!gStarted) { gEnabled = false; setStatus(State::Off, "radio failed"); return; }
        setStatus(State::Idle, "");
    } else {
        UiCmd c = {};
        c.type = UiCmd::Cancel;
        postUi(c);
        // Let the task notice gEnabled and tear the session down first.
        vTaskDelay(pdMS_TO_TICKS(120));
        radioDown();
        setStatus(State::Off, "");
    }
}

bool enabled() { return gEnabled; }

std::vector<Peer> peers() {
    std::vector<Peer> out;
    lock();
    for (size_t i = 0; i < gPeerCount; i++) out.push_back(gPeers[i]);
    unlock();
    return out;
}

bool sendTo(const uint8_t mac[6], const std::string &path) {
    if (!gEnabled || !mac || path.empty() || path.size() >= 160) return false;
    UiCmd c = {};
    c.type = UiCmd::SendTo;
    memcpy(c.mac, mac, 6);
    strncpy(c.path, path.c_str(), sizeof(c.path) - 1);
    postUi(c);
    return true;
}

bool verificationCode(char out[7]) {
    if (!out) return false;
    lock();
    bool ready = gStatus.state == State::Verifying;
    unlock();
    if (!ready) return false;
    snprintf(out, 7, "%06u", (unsigned)(gSess.code % 1000000u));
    return true;
}

void confirmCode(bool matches) {
    UiCmd c = {};
    c.type = UiCmd::ConfirmCode;
    c.flag = matches;
    postUi(c);
}

bool pendingOffer(Offer &out) {
    lock();
    bool have = gOfferPending;
    if (have) out = gOffer;
    unlock();
    return have;
}

void respondToOffer(bool accept) {
    UiCmd c = {};
    c.type = UiCmd::RespondOffer;
    c.flag = accept;
    postUi(c);
}

void cancel() {
    UiCmd c = {};
    c.type = UiCmd::Cancel;
    postUi(c);
}

Status status() {
    Status s;
    lock();
    s = gStatus;
    unlock();
    return s;
}

} // namespace squirt
