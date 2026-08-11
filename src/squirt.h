#pragma once
#include <Arduino.h>
#include <string>
#include <vector>

// Squirt: hand the track you're playing to another ES3C28P in the room, over
// the radio, with no network to join and nothing paired in advance.
//
// Every device runs this same code and is symmetric: it beacons, it listens,
// and it can be either end of a transfer. Roles are decided per transfer by
// who taps send.
//
// Transfers are end-to-end encrypted and both users have to confirm a
// six-digit code that appears on both screens before anything moves. See the
// security notes at the top of squirt.cpp for what that does and doesn't buy
// you, and the README for the short version.
namespace squirt {

enum class State : uint8_t {
    Off,          // radio down; the feature is not running at all
    Idle,         // discoverable, listening, nothing in flight
    Handshaking,  // keys being agreed with a peer
    Verifying,    // six-digit code on screen, waiting for both users
    Offering,     // sender: waiting for the other user to accept
    Deciding,     // receiver: an offer is on screen, waiting for this user
    Sending,
    Receiving,
    Complete,
    Failed,
};

struct Peer {
    uint8_t  mac[6]   = {0};
    char     name[24] = {0};
    uint32_t lastSeen = 0;   // millis() of the last beacon heard
};

// What the receiving user is being asked to approve. Sizes are what the
// sender *claims*; the transfer is aborted if the real thing exceeds them.
struct Offer {
    char     from[24]     = {0};
    char     filename[64] = {0};   // already sanitized, safe to display and to use
    uint32_t bytes        = 0;
};

struct Status {
    State    state       = State::Off;
    uint32_t transferred = 0;
    uint32_t total       = 0;
    char     peerName[24] = {0};
    char     message[64]  = {0};   // human-readable detail, especially on failure
};

// Sets the name other devices see and prepares internal state. Does NOT bring
// up the radio -- call setEnabled(true) for that. Safe to call once at boot.
bool begin(const char *deviceName = nullptr);

// Powers the radio and starts beaconing/listening, or shuts it all back down.
// Off by default and worth leaving off: a radio that isn't up can't be
// attacked, and an idle ESP-NOW listener is a real (if small) power draw.
void setEnabled(bool on);
bool enabled();

// Nearby devices heard from recently. Stale entries age out on their own.
std::vector<Peer> peers();

// Offer `path` to `mac`. Returns false if busy, disabled, or the file is
// unsuitable. Success only means the offer was started -- watch status().
bool sendTo(const uint8_t mac[6], const std::string &path);

// --- confirmation, needed on both devices ---------------------------------
// In State::Verifying, both screens show the same six digits if and only if
// they are talking directly to each other. Show it, and pass on what the user
// says. Anything other than a confirmed match aborts the transfer.
bool verificationCode(char out[7]);
void confirmCode(bool matches);

// In State::Deciding, the receiving user approves or rejects the incoming file.
bool pendingOffer(Offer &out);
void respondToOffer(bool accept);

// Abort whatever is in flight, from either end, at any point.
void cancel();

Status status();

// Where accepted files land. Received files are never played automatically
// and never overwrite anything.
const char *inboxPath();

} // namespace squirt
