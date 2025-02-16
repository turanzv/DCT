#ifndef DIST_SGKEY_HPP
#define DIST_SGKEY_HPP
#pragma once
/*
 * dist_sgkey - distribute a pub/priv X keypair to the peers in a bespoke
 * transport using publisher privacy with authorized subscription.
 * This version is a self-contained 'header-only' library.
 *
 * DistSGKey manages all the group key operations including the decision on
 * which (eligible) entity will create the subscriber group key pair. Only one entity should
 * be making group key pairs and will rekey at periodic intervals to distribute
 * a new key, encrypting each private key with the public key of each peer with
 * subscriber capability (see https://libsodium.gitbook.io/doc/advanced/ed25519-curve25519),
 * denoted by "SG" capability. The corresponding public key is not encrypted.
 * If a new subscribing member joins between rekeying, it is added to the member
 * list and a Publication with a secret key encrypted for it is published.
 * The group key pair is used by sigmgr_ppaead.hpp and sigmgr_ppsigned.hp
 *
 * Any entity with a valid certificate for this trust schema (i.e., any entity that belongs to
 * this trust domain) can publish but must have the appropriate SG (denoted by the
 * SG cert's capability argument) in order to subscribe.
 * 
 * Copyright (C) 2020-3 Pollere LLC
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation; either version 2.1 of
 *  the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this program; if not, see <https://www.gnu.org/licenses/>.
 *  You may contact Pollere LLC at info@pollere.net.
 *
 *  dist_sgkey is not intended as production code.
 */

#include <algorithm>
#include <cstring> // for memcmp
#include <functional>
#include <utility>

#include <dct/schema/capability.hpp>
#include <dct/schema/certstore.hpp>
#include <dct/schema/tlv_encoder.hpp>
#include <dct/schema/tlv_parser.hpp>
#include <dct/sigmgrs/sigmgr_by_type.hpp>
#include <dct/syncps/syncps.hpp>
#include <dct/utility.hpp>
#include "km_election.hpp"

namespace dct {

using namespace std::literals::chrono_literals;

struct DistSGKey {
    using connectedCb = std::function<void(bool)>;

    static constexpr uint32_t kxpkKeySz = crypto_kx_PUBLICKEYBYTES;
    static constexpr uint32_t kxskKeySz = crypto_kx_SECRETKEYBYTES;
    static constexpr size_t encSGKeySz = crypto_box_SEALBYTES + kxskKeySz;
    using encSGK = std::array<uint8_t, crypto_box_SEALBYTES + kxskKeySz>;
    using xmpk = std::array<uint8_t, crypto_scalarmult_curve25519_BYTES>;
    using addKeyCb = std::function<void(keyRef, keyRef, uint64_t)>;
    using kmpriCB = ofats::any_invocable<int32_t(thumbPrint)>;
    using sgmCB = ofats::any_invocable<bool(thumbPrint)>;

    /*
     * DistSGKey Publications contain the creation time of the group key pair (an 8 byte
     * uint64_t), the pair public key (kxpkKeySz) and a list containing the pair secret key
     * individually encrypted for each authorized subscriber peer. The list holds the
     * thumbprint of a signing key and the group secret key encrypted using that (public)
     * signing key. Publication names contain the range of thumbprints contained in the
     * enclosed list. (96 bytes also accounts for tlv indicators and sigInfo)
     */
    using egkr = std::pair<const thumbPrint, encSGK>;
    //max key records per Pub
    static constexpr int maxKR = (maxPubSize - kxpkKeySz - 8 - 96) / (sizeof(thumbPrint) + encSGKeySz);

    const crName m_prefix;     // prefix for pubs in this distributor's collection
    const crName m_krPrefix;     // prefix for subscriber group key pair records publications
    const crName m_mrPrefix;     // prefix for subscriber group membership request publications
    const std::string m_keyColl;    // the key subCollection handled by this distributor
    SigMgrAny m_syncSM{sigMgrByType("EdDSA")};  // to sign/validate SyncData packets
    SigMgrAny m_keySM{sigMgrByType("EdDSA")};   // to sign/validate Publications in key Collection
    SyncPS m_sync;
    const certStore& m_certs;
    addKeyCb m_newKeyCb;   // called when subscriber group key pair rcvd
    connectedCb m_connCb{[](auto) {}};
    kmpriCB m_kmpri;   // to check a signing chain for key maker capability
    sgmCB m_sgMem;    //to check signing chain for sub group capability for this keys subcollection
    Cap::capChk m_sgCap;   // routine to check a signing chain for subscriber capability
    thumbPrint m_tp{};
    thumbPrint m_kmtp{};
    keyVal m_pDecKey{}; //local public signing key converted to X and
    keyVal m_sDecKey{}; // local signing sk converted to X (used by subr to open sealed box with subscriber group secret key)
    keyVal m_sgSK{};    // current subscribergroup secret key: made and kept by keymaker
    keyVal m_sgPK{};    // current subscribergroup public key: made and kept by keymaker
    uint64_t m_curKeyCT{};      // current sg key pair creation time in microsecs
    std::map<thumbPrint,xmpk> m_mbrList{};
    std::chrono::milliseconds m_reKeyInt{3600};
    std::chrono::milliseconds m_keyRand{10};
    std::chrono::milliseconds m_keyLifetime{3600+10};
    kmElection* m_kme{};
    uint32_t m_KMepoch{};      // current election epoch
    bool m_keyMaker{false};     // true if this entity is a key maker
    bool m_subr{false};         // set if this identity has the subscriber capability
    bool m_init{true};          // key maker status unknown while in initialization
    bool m_pubdist{false};        // true indicates this is a pub group key distributor (not pdu)
    bool m_mrPending{false};    //member request pending
    pTimer m_mrRefresh{std::make_shared<Timer>(getDefaultIoContext())}; // to refresh timed out member request

    DistSGKey(DirectFace& face, const Name& pPre, const Name& dPre, addKeyCb&& sgkeyCb, const certStore& cs,
             std::chrono::milliseconds reKeyInterval = std::chrono::seconds(3600),
             std::chrono::milliseconds reKeyRandomize = std::chrono::seconds(10),
             std::chrono::milliseconds expirationGB = std::chrono::seconds(60)) :
             m_prefix{pPre}, m_krPrefix{pPre/"kr"}, m_mrPrefix{pPre/"mr"}, m_keyColl{dPre.last().toSv()},
             m_sync(face, dPre, m_syncSM.ref(), m_keySM.ref()),
             m_certs{cs}, m_newKeyCb{std::move(sgkeyCb)}, //called when a (new) group key arrives or is created      
             m_sgCap{Cap::checker("SG", pPre, cs)},
             m_reKeyInt(reKeyInterval), m_keyRand(reKeyRandomize),
             m_keyLifetime(m_reKeyInt + m_keyRand) {
        m_sync.cStateLifetime(253ms);
        m_sync.pubLifetime(std::chrono::milliseconds(reKeyInterval + reKeyRandomize + expirationGB));
        m_sync.getLifetimeCb([this,cand=crPrefix(m_prefix/"km"/"cand"),mreq=crPrefix(m_mrPrefix)](const auto& p) {
                if (mreq.isPrefix(p.name())) return 6000ms; //0ms;
                return cand.isPrefix(p.name())? 1000ms : m_keyLifetime;
            });
#if 0
        // Order publications by ours first then most recent first
        m_sync.filterPubsCb([](auto& pOurs, auto& pOthers) {
                if (pOurs.empty()) return; // non-keymakers don't reply
                const auto cmp = [](const auto& p1, const auto& p2) {
                    return p1.name().last().toTimestamp() > p2.name().last().toTimestamp();
                };
                if (pOurs.size() > 1) std::sort(pOurs.begin(), pOurs.end(), cmp);
                if(! pOthers.empty()) {
                    std::sort(pOthers.begin(), pOthers.end(), cmp);
                    for (auto& p : pOthers) pOurs.push_back(p);
                }
            });
#endif

        // get our identity thumbprint, check if we're allowed to make keys, check if we
        // are in subscriber group, then set up our public and private signing keys.
        m_tp = m_certs.Chains()[0];
        if ( m_sync.collName_.last().toSv() == "pubs") m_pubdist = true;
        updateSigningKey(m_certs.key(m_tp), m_certs[m_tp]);
    }

    // publish my membership request with updated key: name <m_mrPrefix><timestamp>
    // requests don't have epoch since the keymaker sets the epoch, member learns from key list
    void publishMembershipReq() {
        m_mrRefresh->cancel();  // if a membership request refresh is scheduled, cancel it
        if(!m_subr) return;     // don't have permission to be a member
        crData p(m_mrPrefix/std::chrono::system_clock::now());
        p.content(std::vector<uint8_t>{});
        m_keySM.sign(p);    // will put my thumbprint into Publication
        m_mrPending = true;
        m_sync.publish(std::move(p));
        m_mrRefresh = m_sync.schedule(m_keyLifetime, [this](){ publishMembershipReq(); });
    }

    // Called when a group key has been received and decrypted. Cancel any pending refresh
    // of the membership request. It will be reissued only if a new group key record is received
    // and I'm not in the list
    void receivedGK() {
        m_mrRefresh->cancel();  // if a membership request refresh is scheduled, cancel it
        m_mrPending = false;
    }
    /*
     * Called to process a new local signing key. Passes to the SigMgrs.
     * Stores the thumbprint and makes decrypt versions of the public
     * key and the secret key to use to decrypt the group key.
     * Creates X version of group member's signing key for encrypt/decrypt
     * Currently only called at start up but this would need to be called (likely through dct_model)
     * if a local signing key pair is updated after start up.
     */
       /*
     * Called to process a new local signing key. Passes to the SigMgrs.
     * Stores the thumbprint and makes decrypt versions of the public
     * key and the secret key to use to decrypt the group key.
     *      use new key immediately to sign - update the my signature managers
     *      if member, send a new membership request
     *      need to keep immediately prior decrypt key
     */
    void updateSigningKey(const keyVal sk, const rData& pubCert) {
        // check this new key matches 0th signing chain
        m_tp = m_certs.Chains()[0];
        if (m_tp != dctCert::computeThumbPrint(pubCert))
            throw runtime_error("dist_sgkey:updateSigningKey gets new key not at chains[0]");

        // sigmgrs need to get the new signing keys and public key lookup callbacks
        m_syncSM.updateSigningKey(sk, pubCert);
        m_keySM.updateSigningKey(sk, pubCert);
        m_syncSM.setKeyCb([&cs=m_certs](rData d) -> keyRef { return cs.signingKey(d); });
        m_keySM.setKeyCb([&cs=m_certs](rData d) -> keyRef { return cs.signingKey(d); });

        // build function to get the subscriber group id from a signing chain
        if (m_init) {
            auto sgId = Cap::getval("SG", m_prefix, m_certs);
            //checks if SG cap is present and, if so, returns its argument
            // return 0 if cap wasn't found or has wrong content
            // eventually use an m_keyColl that can have a subcollection within pubs
            m_sgMem = [this,sgId](const thumbPrint& tp) { return sgId(tp).toSv() == m_keyColl; };
        }
        if( m_subr && !m_sgMem(m_tp) )  // first time through m_subr will be false for all
            std::runtime_error("DistSGKey::updateSigningKey subscriber group capability change indicates bad signing chain");
        m_subr = m_sgMem(m_tp);     // set from signing chain
        if (! m_subr)  return;       // this identity is publish only, done updating signing pair

        // this member has subscriber group capability
        // convert the new key to form needed for group key encrypt/decrypt
        //only need first 32 bytes of sk (rest is appended pk)
        keyVal ssk(sk.begin(), sk.begin()+32);
        m_sDecKey.resize(crypto_scalarmult_curve25519_BYTES);
        if(crypto_sign_ed25519_sk_to_curve25519(m_sDecKey.data(), ssk.data()) != 0) {
            std::runtime_error("DistSGKey::updateSigningKey could not convert secret key");
        }
        m_pDecKey.resize(crypto_scalarmult_curve25519_BYTES);
        const auto& pk = pubCert.content().toSpan();
        if(crypto_sign_ed25519_pk_to_curve25519(m_pDecKey.data(), pk.data()) != 0) {
            std::runtime_error("DistSGKey::updateSigningKey unable to convert signing pk to sealed box pk");
        }

        if (m_init) return;
        if (! m_keyMaker) {
             publishMembershipReq();
             return;
        }
        if (m_kmpri(m_tp) > 0) {
            m_kmtp = m_tp;
        } else
            std::runtime_error("DistSGKey::updateSigningKey keymaker capability change indicates bad signing chain");
    }

    void initDone() {
        if (m_init) {
            m_init = false;
      //      m_sync.cStateLifetime(6763ms);
            m_connCb(true);
        }
    }

    /*
     * Called when a new Publication is received in the Key Record topic
     * If have subr capability, look for the group key record with *my* key thumbprint
     * Using first 4 bytes of thumbPrints as identifiers. In the unlikely event that the first and last
     * thumbPrint identifiers are the same, doesn't really matter since we look through for our full
     * thumbPrint and just return if don't find it
     * kr names <m_krPrefix><epoch><low tpId><high tpId><timestamp>
     */
    void receiveSGKeyRecords(const rPub& p)
    {
        if (m_kmpri(p.thumbprint()) <= 0) {
            print("ignoring keylist signed by unauthorized identity {}\n", m_certs[p].name());
            return;
        }
        if (m_keyMaker) {
            // another member claims to be a keyMaker - largest thumbPrint wins
            if (m_tp < p.thumbprint()) {
                    print("keymaker got keylist from {}\n", m_certs[p].name());
                    m_keyMaker = false;
                    m_kmtp = p.thumbprint();
                    m_sync.unsubscribe(m_mrPrefix);
                    publishMembershipReq();
            }
            return;
        }
        if(m_init && m_subr && !m_mrPending) {
            publishMembershipReq();
            return;
        }
        /*
         *  I am a member and have an outstanding membership request that this krlist may service.
         * Parse the name and make checks to determine if this key record publication should be used.
         * if this msg was from an earlier Key Maker epoch, ignore it. If from a later
         * epoch, wrongEpoch will cancel any in-progress election and update our epoch.
         */
        auto n = p.name();
        auto epoch = n.nextAt(m_krPrefix.size()).toNumber();
        if (epoch != m_KMepoch) {
            if (epoch > 1) { // XXX change this when re-elections supported
                print("keylist ignored: bad epoch {} in {} from {}\n", epoch, p.name(), m_certs[p].name());
                return;
            }
            //assert(m_KMepoch == 0);
            m_KMepoch = epoch;
            m_kmtp.fill(0);     //new epoch, reset my record of keymaker tp
        }
        // check if keymaker has a larger tp than my stored value (can resolve conflict after elections this can happen in
        // relayed domains in particular), if so, (re)set my saved value and cur key ct so I get a new key
        if (m_kmtp < p.thumbprint())    { m_curKeyCT = 0; m_kmtp = p.thumbprint(); }

        // if I'm a subscriber, check if I'm included in this pub
        static constexpr auto less = [](const auto& a, const auto& b) -> bool {
            auto asz = a.size();
            auto bsz = b.size();
            auto r = std::memcmp(a.data(), b.data(), asz <= bsz? asz : bsz);
            if (r == 0) return asz < bsz;
            return r < 0;
        };

        /*
         * Decode the Content to extract key pair creation time, public key and vector
         * of encrypted secret keys if applicable
         */
        decltype(m_curKeyCT) newCT{};
        auto tpl = n.nextBlk().toSpan();
        auto tph = n.nextBlk().toSpan();
        auto tpId = std::span(m_tp).first(tpl.size());
        if(m_subr && (less(tpId, tpl) || less(tph, tpId))) {
            //no secret key for me in this pub
            if (std::cmp_less(m_curKeyCT, newCT) && !m_mrPending) {
                // new key is being published, make sure keymaker has my membership
                m_sync.oneTime(2000ms, [this](){ publishMembershipReq(); } ) ;
            }
            return;
        }
        std::span<const uint8_t> sgPK{};
        std::span<const uint8_t> sgSK{};
        std::span<const egkr> gkrVec{};
        try {
            auto content = p.content();
            // the first tlv should be type 36 and it should decode to a uint64_t
            newCT = content.nextBlk(36).toNumber();
            // a new key will have a creation time larger than m_curKeyCT
            if(newCT <= m_curKeyCT) {
                return; //received key is not newer than current key
            }

            // the second tlv should be type 150 and should decode to a vector of uint8_t (for public SG key)
            sgPK = content.nextBlk(150).toSpan();
            if(!m_subr) {   //I'm not a subscriber, just get public key
                m_curKeyCT = newCT;
                m_newKeyCb(sgPK, sgSK, m_curKeyCT); //use addKeyCb to set new sg public key in pub privacy sigmgr
                if (m_init) initDone();
                return;
            }
            // the third tlv should be type 130 and should be a vector of gkr pairs
            gkrVec = content.nextBlk(130).toSpan<egkr>();
            // (future: ensure it's from the same creator as last time?)
        } catch (std::runtime_error& ex) {
            return; //ignore this groupKey message - content type error e.what()
        }

        /*
         * Subscriber group member looks for encrypted secret key
         */
        auto it = std::find_if(gkrVec.begin(), gkrVec.end(), [this](auto p){ return p.first == m_tp; });
        if (it == gkrVec.end()) return; //didn't find my key in this Publication

        const auto& nk = it->second;    //decrypt and save the secret key of pair [might not need to save since gets passed to sigmgr to use]
        uint8_t m[kxskKeySz];
        if(crypto_box_seal_open(m, nk.data(), nk.size(), m_pDecKey.data(), m_sDecKey.data()) != 0) {
            return; //can't open encrypted key
        }
        //Received a good key pair, now can set it
        sgSK = std::span<uint8_t>(m, m + kxskKeySz);
        m_curKeyCT = newCT;
        m_newKeyCb(sgPK, sgSK, m_curKeyCT);   //call back parent to pass the new sg key pair to pub privacy sigmgr
        receivedGK();   // got new sg key, cancel pending member request and schedule one for key lifetime
        if (m_init) initDone(); // have a key, can exit initialization
    }

    /*
     * setup() is called from a start function in dct_model,
     * after some initial signing certs have been exchanged so it's known
     * there are active peers. It is passed a callback, 'ccb', to be
     * invoked when this entity has completed initialization. For a keymaker, that means
     * winning the election, making the first group key and having some entity receive
     * its key record (it does not need to have sg members since it may be the only subscriber
     * but does need to know a potential publisher got the public key).
     * For a non-keymaker sg member, it must have received the group key pair.
     * For a pure publisher, it must have received the public key. The value of the keyMaker
     * capability influences the probability of this entity being elected
     * with larger values giving higher priority.
     * A trust schema using subscription groups should ensure key maker
     * capability is only given with subscription group capability.
     */

    void setup(connectedCb&& ccb) {
        m_connCb = std::move(ccb);
        if ( m_sync.collName_.last().toSv() == "pubs") m_pubdist = true;    // this will need to be changed if put in names for subscriber groups
        // XXXX Hack to keep a relay from participating in pub group since it doesn't do encryption or decryption
        if (m_pubdist && m_certs[m_tp].name()[1].toSv() == "relay"s) {
            initDone();
            return;
        }

        // build function to get the key maker priority from a signing chain then
        // use it to see if we should join the key maker election
        auto kmval = Cap::getval(m_pubdist ? "KMP" : "KM", m_prefix, m_certs);
        auto kmpri = [kmval](const thumbPrint& tp) {
                          // return 0 if cap wasn't found or has wrong content
                          // XXX value currently has to be a single digit
                          auto kmv = kmval(tp);
                          if (kmv.size() != 3) return 0;
                          auto c = kmv.cur();
                          if (c < '0' || c > '9') return 0;
                          return c - '0';
                      };
        m_kmpri = kmpri;

        if(m_subr && m_kmpri(m_tp) > 0 ) {
            auto eDone = [this](auto elected, auto epoch) {
                            m_keyMaker = elected;
                            m_KMepoch = epoch;
                            // all members  subscribe to group key subcollection; keymakers subscribe in case of conflicts
                            m_sync.subscribe(m_krPrefix, [this](const auto& p){ receiveSGKeyRecords(p); });
                            if (! elected)  return;
                            // keymaker must get requests
                            m_sync.subscribe(m_mrPrefix, [this](const auto& p){ addGroupMem(p); });
                            sgkeyTimeout();  //create a group key, publish it, callback parent with key
                          };
            m_kme = new kmElection(m_prefix/"km", m_keySM.ref(), m_sync, std::move(eDone), std::move(kmpri), m_tp, m_pubdist? 5s : 500ms);
        } else {    // non-keymaker subscribers and pure publishers
             m_sync.subscribe(m_krPrefix, [this](const auto& p){ receiveSGKeyRecords(p); });
        }
    }

    /*** Following methods are used by the keymaker to distribute and maintain the group key records ***/

    // Publish the subscriber group key list from thumbpring tpl to thumbprint tph
   // kr names <m_krPrefix><epoch><low tpId><high tpId><timestamp>
    void publishKeyRange(auto& tpl, auto& tph, auto ts, auto& c, bool conf=false) {
        //constant 4 can be determined dynamically later
        const auto TP = [](const auto& tp){ return std::span(tp).first(4); };
        crData p(m_krPrefix/m_KMepoch/TP(tpl)/TP(tph)/ts);
        p.content(c);
        m_keySM.sign(p);
        if(conf) {
            // this keymaker has no subscribers, but at least one publish-capable member
            m_sync.publish(std::move(p), [this](const auto&, bool f){ if(f) initDone(); });
        } else m_sync.publish(std::move(p));
    }

    // Make a new subscriber key pair, publish it, and locally switch to using the new key.

    void makeSGKey() {
        //make a new key pair
        m_sgPK.resize(kxpkKeySz);
        m_sgSK.resize(kxskKeySz);
        crypto_kx_keypair(m_sgPK.data(), m_sgSK.data());    //X25519
        //set the creation time
        m_curKeyCT = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();

        // remove expired (not valid) certs (thumbprints) from memberList
        auto now = std::chrono::system_clock::now();
        std::erase_if(m_mbrList, [this,now](auto& kv) { return m_certs.contains(kv.first)? rCert(m_certs[kv.first]).validUntil() <= now : true; });

        //encrypt the new secret key for all the subscriber group members
        std::vector<egkr> pubPairs;
        for (auto& [k,v]: m_mbrList) {
            encSGK egKey;
            crypto_box_seal(egKey.data(), m_sgSK.data(), m_sgSK.size(), v.data());
            pubPairs.emplace_back(k,egKey);
        }
        // call back to parent with new key, parent calls the application publication sigmgr's addKey()
        m_newKeyCb(m_sgPK, m_sgSK, m_curKeyCT);

        auto s = m_mbrList.size();
        auto p = s <= maxKR ? 1 : (s + maxKR - 1) / maxKR; // determine number of Publications needed
        auto pubTS = std::chrono::system_clock::now();
        auto it = pubPairs.begin();
        for(auto i=0u; i<p; ++i) {
            auto r = i < (p-1) ? maxKR : s;
            tlvEncoder sgkp{};    //tlv encoded content
            sgkp.addNumber(36, m_curKeyCT);
            sgkp.addArray(150, m_sgPK);
            if(m_mbrList.size()==0) {
                // publish empty secret key record with public cert to continue to assert keyMaker role and to get key to publishers
                sgkp.addArray(130, pubPairs);
                // use own tp in range and indicate publish with confirmation callback
                publishKeyRange(m_tp, m_tp, pubTS, sgkp.vec(), true);
                break;
            }
            it = sgkp.addArray(130, it, r);
            auto l = i*maxKR;
            publishKeyRange(pubPairs[l].first, pubPairs[l+r-1].first, pubTS, sgkp.vec());
            s -= r;
        }

        if(m_init && m_mbrList.size())  initDone();
    }

    // Periodically refresh the group key. This routine should only be called *once*
    // since each call will result in an additional refresh cycle running.
    void sgkeyTimeout() {
        if (!m_keyMaker) return;    // since not a cancelable timer, need to stop if I lose a future election
        makeSGKey();
        m_sync.oneTime(m_reKeyInt, [this](){ sgkeyTimeout();});  //next re-keying event
    }

    /*
     * This called when there is a new valid peer member request to join the distribution group.
     * This indicates to the keyMaker there is a new peer that needs the group key.
     * Only subscribe to this after win election to be keyMaker, ignore if not a keyMaker as a safeguard
     * If received while in initialization state, haven't made a key yet so don't try to publish.
     *
     * This called when there is a new valid peer member request to join the subscriber group.
     * This indicates to the keyMaker there is a new peer that needs the group secret key
     * Only subscribe to this after win election to be keyMaker, ignore if not a keyMaker as a safeguard
     * If received while in initialization state, haven't made a key yet so don't try to publish.
     *
     * Shouldn't have to republish the entire list of key records, so publishes a new encrypted secret key separately.
     * Might also want to check if this peer is de-listed (e.g., blacklisted) but for now assuming
     * this is handled in validation of cAdd PDU and of Publications
     * A publish-only member can get the public key from any kr Publication
     */
    void addGroupMem(const rData& p) {
        if (!m_keyMaker) return;

        // number of Publications should be fewer than 'complete peeling' iblt threshold (currently 80).
        // Each gkR is ~100 bytes so the default maxPubSize of 1024 allows for ~800 members. 
        // Future: return some indication of this
        if (m_mbrList.size() == 80*maxKR)   return;

        auto tp = p.thumbprint();
        if(! m_sgMem(tp)) return;  //this signing cert doesn't have SG capability.
        // XXXX Test here for request from relay role in /keys/pubs/mr (later would be rejected in validation)
        if(m_pubdist && m_certs[tp].name()[1].toSv() == "relay"s)  return;    //this is a hacky hack

        auto pk = m_certs[tp].content().toVector();   //access the public key for this signer's thumbPrint
        // convert pk to form that can be used to encrypt and add to member list
        if(crypto_sign_ed25519_pk_to_curve25519(m_mbrList[tp].data(), pk.data()) != 0) {
            m_mbrList.erase(tp);    //unable to convert member's pk to sealed box pk
            return;
        }
        if(!m_curKeyCT)    return;  // haven't made first group key

        //publish the subscriber group key for this new peer
        encSGK egKey;
        crypto_box_seal(egKey.data(), m_sgSK.data(), m_sgSK.size(), m_mbrList[tp].data());
        std::vector<egkr> ekp {{tp,egKey}};
        tlvEncoder sgkp{};    //tlv encoded content
        sgkp.addNumber(36, m_curKeyCT);
        sgkp.addArray(150, m_sgPK);
        sgkp.addArray(130, ekp);
        publishKeyRange(tp, tp, std::chrono::system_clock::now(), sgkp.vec());

        if (m_init) initDone();    // Have a keyMaker, a group key, and at least one member: exit init state
    }

    /*
     *  won't encrypt a group key for this thumbPrint in future
     * if this becomes a subscription callback for delisted publications, should
     * probably mark mbrList entries rather than delete
     * if reKey is set, change the group key now to exclude the removed member
     */
    void removeGroupMem(thumbPrint& tp, bool reKey = false) {
        m_mbrList.erase(tp);
        if(reKey) makeSGKey(); //issue new key without disturbing rekey schedule
    }
};

}   // namespace dct

#endif //DIST_SGKEY_HPP

