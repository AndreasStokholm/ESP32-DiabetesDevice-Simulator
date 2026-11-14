#include "crypto.h"

#include <mbedtls/aes.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <string.h>

// The party identifiers the sensor and client sign their proofs with.
static const uint8_t sensorParty[] = {0x37, 0x56, 0x27, 0x67, 0x56, 0x27};
static const uint8_t clientParty[] = {'c', 'l', 'i', 'e', 'n', 't'};

namespace {

// RAII wrappers keep the mbedTLS bookkeeping out of the protocol code.
struct Mpi {
  mbedtls_mpi v;
  Mpi() { mbedtls_mpi_init(&v); }
  ~Mpi() { mbedtls_mpi_free(&v); }
  mbedtls_mpi* operator&() { return &v; }
};
struct Point {
  mbedtls_ecp_point p;
  Point() { mbedtls_ecp_point_init(&p); }
  ~Point() { mbedtls_ecp_point_free(&p); }
  mbedtls_ecp_point* operator&() { return &p; }
};

void loadCurve(mbedtls_ecp_group* g) {
  mbedtls_ecp_group_init(g);
  mbedtls_ecp_group_load(g, MBEDTLS_ECP_DP_SECP256R1);
}

// k in [1, N-1]: random mod (N-2), plus one.
void randomScalar(mbedtls_ecp_group* g, mbedtls_mpi* k) {
  uint8_t b[32];
  esp_fill_random(b, sizeof b);
  Mpi upper;
  mbedtls_mpi_sub_int(&upper, &g->N, 2);
  mbedtls_mpi_read_binary(k, b, sizeof b);
  mbedtls_mpi_mod_mpi(k, k, &upper);
  mbedtls_mpi_add_int(k, k, 1);
}

void mul(mbedtls_ecp_group* g, mbedtls_ecp_point* r, const mbedtls_mpi* k, const mbedtls_ecp_point* p) {
  int rc = mbedtls_ecp_mul(g, r, k, p, hwRng, nullptr);
  if (rc) logf("ecp_mul failed: -0x%04x", -rc);
}

void add(mbedtls_ecp_group* g, mbedtls_ecp_point* r, const mbedtls_ecp_point* p, const mbedtls_ecp_point* q) {
  Mpi one;
  mbedtls_mpi_lset(&one, 1);
  int rc = mbedtls_ecp_muladd(g, r, &one, p, &one, q);
  if (rc) logf("ecp_muladd failed: -0x%04x", -rc);
}

void neg(mbedtls_ecp_group* g, mbedtls_ecp_point* p) { mbedtls_mpi_sub_mpi(&p->Y, &g->P, &p->Y); }

void pad32(const mbedtls_mpi* v, uint8_t* out) { mbedtls_mpi_write_binary(v, out, 32); }

void uncompressed(mbedtls_ecp_group* g, const mbedtls_ecp_point* p, uint8_t out[65]) {
  size_t n = 0;
  mbedtls_ecp_point_write_binary(g, p, MBEDTLS_ECP_PF_UNCOMPRESSED, &n, out, 65);
}

void transcriptHash(mbedtls_ecp_group* g, const mbedtls_ecp_point* base, const mbedtls_ecp_point* proof,
                    const mbedtls_ecp_point* pub, const uint8_t* party, size_t partyLen, mbedtls_mpi* out) {
  uint8_t enc[3][65];
  uncompressed(g, base, enc[0]);
  uncompressed(g, proof, enc[1]);
  uncompressed(g, pub, enc[2]);
  mbedtls_sha256_context h;
  mbedtls_sha256_init(&h);
  mbedtls_sha256_starts_ret(&h, 0);
  auto part = [&](const uint8_t* p, uint32_t n) {
    uint8_t l[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    mbedtls_sha256_update_ret(&h, l, 4);
    mbedtls_sha256_update_ret(&h, p, n);
  };
  for (auto& e : enc) part(e, 65);
  part(party, partyLen);
  uint8_t sum[32];
  mbedtls_sha256_finish_ret(&h, sum);
  mbedtls_sha256_free(&h);
  mbedtls_mpi_read_binary(out, sum, 32);
  mbedtls_mpi_mod_mpi(out, out, &g->N);
}

// A public key and a Schnorr proof of knowledge of its private key.
Bytes makeCert(mbedtls_ecp_group* g, const mbedtls_ecp_point* base, const mbedtls_mpi* priv) {
  Mpi v, c, r, t;
  Point proof, pub;
  randomScalar(g, &v);
  mul(g, &proof, &v, base);
  mul(g, &pub, priv, base);
  transcriptHash(g, base, &proof, &pub, sensorParty, sizeof sensorParty, &c);
  mbedtls_mpi_mul_mpi(&t, &c, priv);
  mbedtls_mpi_sub_mpi(&r, &v, &t);
  mbedtls_mpi_mod_mpi(&r, &r, &g->N);

  Bytes out(PCertLength);
  pad32(&pub.p.X, &out[0]);
  pad32(&pub.p.Y, &out[32]);
  pad32(&proof.p.X, &out[64]);
  pad32(&proof.p.Y, &out[96]);
  pad32(&r.v, &out[128]);
  return out;
}

bool decodeCert(mbedtls_ecp_group* g, const Bytes& b, mbedtls_ecp_point* pub, mbedtls_ecp_point* proof,
                mbedtls_mpi* r) {
  if (b.size() != PCertLength) return false;
  mbedtls_mpi_read_binary(&pub->X, &b[0], 32);
  mbedtls_mpi_read_binary(&pub->Y, &b[32], 32);
  mbedtls_mpi_lset(&pub->Z, 1);
  mbedtls_mpi_read_binary(&proof->X, &b[64], 32);
  mbedtls_mpi_read_binary(&proof->Y, &b[96], 32);
  mbedtls_mpi_lset(&proof->Z, 1);
  mbedtls_mpi_read_binary(r, &b[128], 32);
  return mbedtls_ecp_check_pubkey(g, pub) == 0;
}

bool verifyCert(mbedtls_ecp_group* g, const mbedtls_ecp_point* base, const mbedtls_ecp_point* pub,
                const mbedtls_ecp_point* proof, const mbedtls_mpi* r) {
  Mpi c;
  Point got;
  transcriptHash(g, base, proof, pub, clientParty, sizeof clientParty, &c);
  if (mbedtls_ecp_muladd(g, &got, r, base, &c, pub) != 0) return false;
  return mbedtls_ecp_point_cmp(&got, proof) == 0;
}

}  // namespace

// MARK: - JPAKE

JPAKE::JPAKE(const std::string& code) {
  loadCurve(&grp);
  mbedtls_mpi_init(&pin);
  mbedtls_mpi_init(&x3);
  mbedtls_mpi_init(&x4);
  mbedtls_ecp_point_init(&X1);
  mbedtls_ecp_point_init(&X2);
  mbedtls_mpi_read_binary(&pin, (const uint8_t*)code.data(), code.size());
  randomScalar(&grp, &x3);
  randomScalar(&grp, &x4);
}

JPAKE::~JPAKE() {
  mbedtls_ecp_point_free(&X1);
  mbedtls_ecp_point_free(&X2);
  mbedtls_mpi_free(&pin);
  mbedtls_mpi_free(&x3);
  mbedtls_mpi_free(&x4);
  mbedtls_ecp_group_free(&grp);
}

Bytes JPAKE::round1() { return makeCert(&grp, &grp.G, &x3); }
Bytes JPAKE::round2() { return makeCert(&grp, &grp.G, &x4); }

bool JPAKE::setClientRound(int round, const Bytes& cert, bool* verified) {
  Point pub, proof;
  Mpi r;
  if (!decodeCert(&grp, cert, &pub, &proof, &r)) return false;
  *verified = verifyCert(&grp, &grp.G, &pub, &proof, &r);
  if (round == 0) {
    mbedtls_ecp_copy(&X1, &pub);
    have1 = true;
  } else if (round == 1) {
    mbedtls_ecp_copy(&X2, &pub);
    have2 = true;
  } else {
    return false;
  }
  return true;
}

void JPAKE::blindedKey(mbedtls_mpi* out) {
  mbedtls_mpi_mul_mpi(out, &x4, &pin);
  mbedtls_mpi_mod_mpi(out, out, &grp.N);
}

// Round 3's base is X1 + X2 + X3.
bool JPAKE::round3(Bytes& out) {
  if (!have1 || !have2) return false;
  Point x3G, sum, base;
  Mpi bk;
  mul(&grp, &x3G, &x3, &grp.G);
  add(&grp, &sum, &X1, &X2);
  add(&grp, &base, &sum, &x3G);
  blindedKey(&bk);
  out = makeCert(&grp, &base, &bk);
  return true;
}

// K = (A - X2·x4·s)·x4; the key is the first 16 bytes of SHA-256(K.x).
bool JPAKE::sharedKey(const Bytes& clientRound3, Bytes& key) {
  if (!have2) return false;
  Point a, proof, t, s, k;
  Mpi r, bk;
  if (!decodeCert(&grp, clientRound3, &a, &proof, &r)) return false;
  blindedKey(&bk);
  mul(&grp, &t, &bk, &X2);
  neg(&grp, &t);
  add(&grp, &s, &a, &t);
  mul(&grp, &k, &x4, &s);
  uint8_t kx[32], sum[32];
  pad32(&k.p.X, kx);
  mbedtls_sha256_ret(kx, 32, sum, 0);
  key.assign(sum, sum + 16);
  return true;
}

Bytes encryptChallenge(const uint8_t* challenge8, const Bytes& key16) {
  uint8_t in[16], out[16];
  memcpy(in, challenge8, 8);
  memcpy(in + 8, challenge8, 8);
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, key16.data(), 128);
  mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, in, out);
  mbedtls_aes_free(&aes);
  return Bytes(out, out + 8);
}

// MARK: - Identity

namespace identity {

// g7sim's fixed CA signing key. Public by design: it marks simulated data,
// it is not a secret.
static const char* dummyCAPrivHex = "7ad3246d555e63b4f7d4ff06bc037a64c108965d0c50036d8a56abfc6c2ea1c7";

// Dexcom's fixed display public key, from G7SensorKit's credentials: a
// legitimate display signs the sensor's key challenge with its private half.
static const char* keyCPubHex =
    "045118C35E9E41E7E0654FEE801C52A9C5DFC510EF09597D5CCA8461E4AF9C666714834F2BC903F16FABFC45755B0183F1A09745CDFFCB4E2F799E50BED9A6B58C";

static std::vector<Bytes> chain;
static uint8_t leaf[32];

static Bytes unhex(const char* s) {
  Bytes b;
  for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
    char t[3] = {s[i], s[i + 1], 0};
    b.push_back(strtoul(t, nullptr, 16));
  }
  return b;
}

static void pkFromScalar(mbedtls_pk_context* pk, const uint8_t d[32]) {
  mbedtls_pk_init(pk);
  mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
  mbedtls_ecp_keypair* kp = mbedtls_pk_ec(*pk);
  mbedtls_ecp_group_load(&kp->grp, MBEDTLS_ECP_DP_SECP256R1);
  mbedtls_mpi_read_binary(&kp->d, d, 32);
  mbedtls_ecp_mul(&kp->grp, &kp->Q, &kp->d, &kp->grp.G, hwRng, nullptr);
}

static Bytes makeCert(mbedtls_pk_context* subject, mbedtls_pk_context* issuer, const char* subjectName,
                      uint64_t serial, bool isCA) {
  mbedtls_x509write_cert crt;
  mbedtls_x509write_crt_init(&crt);
  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&crt, subject);
  mbedtls_x509write_crt_set_issuer_key(&crt, issuer);
  mbedtls_x509write_crt_set_subject_name(&crt, subjectName);
  mbedtls_x509write_crt_set_issuer_name(&crt, "CN=G7SIM-CA");
  Mpi sn;
  uint8_t snb[8];
  for (int i = 0; i < 8; i++) snb[i] = serial >> (56 - 8 * i);
  mbedtls_mpi_read_binary(&sn, snb, 8);
  mbedtls_x509write_crt_set_serial(&crt, &sn);
  // The emulator has no trustworthy clock at boot; the display reads the
  // certificate but does not verify it, so a fixed validity is fine.
  mbedtls_x509write_crt_set_validity(&crt, "20260101000000", "20460101000000");
  mbedtls_x509write_crt_set_basic_constraints(&crt, isCA ? 1 : 0, -1);
  if (isCA) {
    mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_KEY_CERT_SIGN | MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
  }
  static uint8_t buf[1024];
  int n = mbedtls_x509write_crt_der(&crt, buf, sizeof buf, hwRng, nullptr);
  mbedtls_x509write_crt_free(&crt);
  if (n < 0) {
    logf("certificate for %s failed: -0x%04x", subjectName, -n);
    return {};
  }
  return Bytes(buf + sizeof buf - n, buf + sizeof buf);
}

void init(const uint8_t leafKey[32], uint64_t serial) {
  memcpy(leaf, leafKey, 32);
  Bytes caKey = unhex(dummyCAPrivHex);
  mbedtls_pk_context ca, lf;
  pkFromScalar(&ca, caKey.data());
  pkFromScalar(&lf, leafKey);
  char name[40];
  snprintf(name, sizeof name, "CN=G7SIM-%llu", serial);
  chain.clear();
  chain.push_back(makeCert(&ca, &ca, "CN=G7SIM-CA", 1, true));
  chain.push_back(makeCert(&lf, &ca, name, serial, false));
  mbedtls_pk_free(&ca);
  mbedtls_pk_free(&lf);
  logf("identity: CA cert %u bytes, leaf cert %u bytes", (unsigned)chain[0].size(), (unsigned)chain[1].size());
}

void newLeafKey(uint8_t out[32]) {
  mbedtls_ecp_group g;
  loadCurve(&g);
  Mpi d;
  randomScalar(&g, &d);
  pad32(&d.v, out);
  mbedtls_ecp_group_free(&g);
}

const std::vector<Bytes>& certChain() { return chain; }

Bytes signChallenge(const Bytes& displayNonce) {
  uint8_t digest[32];
  mbedtls_sha256_ret(displayNonce.data(), displayNonce.size(), digest, 0);
  mbedtls_ecp_group g;
  loadCurve(&g);
  Mpi d, r, s;
  mbedtls_mpi_read_binary(&d, leaf, 32);
  int rc = mbedtls_ecdsa_sign(&g, &r, &s, &d, digest, 32, hwRng, nullptr);
  mbedtls_ecp_group_free(&g);
  if (rc) logf("ecdsa_sign failed: -0x%04x", -rc);
  Bytes out(64);
  pad32(&r.v, &out[0]);
  pad32(&s.v, &out[32]);
  return out;
}

bool verifyDisplaySignature(const Bytes& challenge, const Bytes& sig) {
  if (sig.size() != 64 || challenge.empty()) return false;
  uint8_t digest[32];
  mbedtls_sha256_ret(challenge.data(), challenge.size(), digest, 0);
  mbedtls_ecp_group g;
  loadCurve(&g);
  Point q;
  Mpi r, s;
  Bytes pub = unhex(keyCPubHex);
  bool ok = mbedtls_ecp_point_read_binary(&g, &q, pub.data(), pub.size()) == 0;
  mbedtls_mpi_read_binary(&r, &sig[0], 32);
  mbedtls_mpi_read_binary(&s, &sig[32], 32);
  ok = ok && mbedtls_ecdsa_verify(&g, digest, 32, &q, &r, &s) == 0;
  mbedtls_ecp_group_free(&g);
  return ok;
}

}  // namespace identity
