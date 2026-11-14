#pragma once
// The sensor's cryptography (port of g7/jpake.go and g7/identity.go):
// EC-JPAKE over P-256 keyed by the 4-digit pairing code, Dexcom's AES
// challenge transform, and the sensor's X.509 identity with a dummy CA.

#include <mbedtls/bignum.h>
#include <mbedtls/ecp.h>

#include "util.h"

// One J-PAKE round on the wire: two P-256 points and a Schnorr proof scalar.
constexpr size_t PCertLength = 160;

// The sensor's side of the pairing exchange over the 4-digit code.
class JPAKE {
 public:
  explicit JPAKE(const std::string& code);
  ~JPAKE();
  JPAKE(const JPAKE&) = delete;
  JPAKE& operator=(const JPAKE&) = delete;

  Bytes round1();
  Bytes round2();
  // Stores the client's round 0 or 1; returns whether its proof verified.
  bool setClientRound(int round, const Bytes& cert, bool* ok);
  bool round3(Bytes& out);
  // The 16-byte AES key both sides arrive at.
  bool sharedKey(const Bytes& clientRound3, Bytes& key);

 private:
  mbedtls_ecp_group grp;
  mbedtls_mpi pin, x3, x4;
  mbedtls_ecp_point X1, X2;  // the client's round 1 and 2 public keys
  bool have1 = false, have2 = false;

  void blindedKey(mbedtls_mpi* out);
};

// Dexcom's challenge transform: the 8 bytes doubled into one AES-128 block,
// ECB, first 8 bytes of the output.
Bytes encryptChallenge(const uint8_t* challenge8, const Bytes& key16);

// The emulated sensor's identity: a leaf P-256 key with a certificate signed by
// g7sim's public dummy CA, so a display can tell it from a genuine sensor.
namespace identity {
// Builds the CA and leaf certificates. Call once at start-up.
void init(const uint8_t leafKey[32], uint64_t serial);
// A fresh random leaf private key for a new sensor.
void newLeafKey(uint8_t out[32]);
// CA at index 0, leaf at index 1, the order a real G7 uses.
const std::vector<Bytes>& certChain();
// ECDSA-P256 over SHA256(nonce) with the leaf key, raw r‖s.
Bytes signChallenge(const Bytes& displayNonce);
// Whether sig is Dexcom's display key's signature over SHA256(challenge).
bool verifyDisplaySignature(const Bytes& challenge, const Bytes& sig);
}  // namespace identity
