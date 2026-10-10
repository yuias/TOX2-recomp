#include "spu/spu_mfc.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <format>
#include <string>

#include "core/reservation.h"

namespace tox2 {
namespace {

constexpr std::uint32_t kLsMask = kTox2SpuLsSize - 1u;
constexpr std::uint32_t kLineMask = kReservationLineSize - 1u;
constexpr std::uint32_t kListStall = 0x80000000u;

const char* commandName(std::uint32_t cmd) {
    switch (cmd) {
        case mfc::kPut: return "put";
        case mfc::kPutB: return "putb";
        case mfc::kPutF: return "putf";
        case mfc::kPutL: return "putl";
        case mfc::kPutLB: return "putlb";
        case mfc::kPutLF: return "putlf";
        case mfc::kGet: return "get";
        case mfc::kGetB: return "getb";
        case mfc::kGetF: return "getf";
        case mfc::kGetL: return "getl";
        case mfc::kGetLB: return "getlb";
        case mfc::kGetLF: return "getlf";
        case mfc::kPutLLUC: return "putlluc";
        case mfc::kPutLLC: return "putllc";
        case mfc::kPutQLLUC: return "putqlluc";
        case mfc::kBarrier: return "barrier";
        case mfc::kMfcEieio: return "mfceieio";
        case mfc::kMfcSync: return "mfcsync";
        case mfc::kGetLLAR: return "getllar";
        case 0xa0:
        case 0xa1:
        case 0xa2: return "sndsig";
        default: return "unknown";
    }
}

std::uint32_t loadBe32(const std::uint8_t* ls, std::uint32_t addr) {
    std::uint32_t v;
    std::memcpy(&v, ls + (addr & kLsMask), sizeof v);
    return std::byteswap(v);
}

void bump(std::atomic<std::uint64_t>& counter) { counter.fetch_add(1, std::memory_order_relaxed); }

}  // namespace

void SpuMfc::stop(std::uint32_t site, std::uint32_t opcode, const Params& p, const char* why) const {
    spuStop(thread_,
            std::format("tox2: stop: spu: mfc {} (command {:#04x}): {} at ls {:#07x} on thread {:#010x}",
                        commandName(opcode), opcode, why, site, thread_.id()),
            std::format("lsa {:#07x} ea {:#010x} size {:#x} tag {}", p.lsa, p.ea, p.size, p.tag));
}

void SpuMfc::checkTransfer(std::uint32_t site, std::uint32_t opcode, const Params& p) const {
    if (p.tag >= mfc::kTagCount) stop(site, opcode, p, "tag id above 31");
    if (p.size == 0 || p.size > mfc::kMaxTransfer) stop(site, opcode, p, "size is 0 or above 0x4000");
    if (std::uint64_t{p.ea} + p.size > (std::uint64_t{1} << 32)) stop(site, opcode, p, "range passes 4 GiB");
    if (p.size >= 16) {
        if ((p.size & 15u) != 0) stop(site, opcode, p, "size is not a multiple of 16");
        if (((p.lsa | p.ea) & 15u) != 0) stop(site, opcode, p, "address is not 16-byte aligned");
        return;
    }
    if (p.size != 1 && p.size != 2 && p.size != 4 && p.size != 8)
        stop(site, opcode, p, "size is not 1, 2, 4, 8 or a multiple of 16");
    if ((p.lsa & (p.size - 1)) != 0 || (p.ea & (p.size - 1)) != 0) stop(site, opcode, p, "address is not aligned");
    if ((p.lsa & 15u) != (p.ea & 15u)) stop(site, opcode, p, "ls and ea differ in the low 4 bits");
}

void SpuMfc::transfer(bool to_ls, std::uint32_t site, std::uint32_t opcode, const Params& p) {
    checkTransfer(site, opcode, p);
    if (to_ls) {
        // Tags the LS for image tracking: bytes of a registered image mark the range, others untag it.
        copyToLs(thread_, base_, p.ea, p.lsa, p.size);
        bump(counters_.get);
        return;
    }
    // Only sizes that are a multiple of 16 can cross the end of the LS; each piece goes through the
    // reservation table so reserved lines see the write.
    std::uint32_t lsa = p.lsa & kLsMask;
    std::uint32_t ea = p.ea;
    std::uint32_t left = p.size;
    while (left != 0) {
        const std::uint32_t n = std::min<std::uint32_t>(left, kTox2SpuLsSize - lsa);
        reservations().put(base_, ea, thread_.ls() + lsa, n);
        ea += n;
        left -= n;
        lsa = 0;
    }
    bump(counters_.put);
}

void SpuMfc::list(bool to_ls, std::uint32_t site, std::uint32_t opcode) {
    const Params head{lsa_, eal_, size_, tag_};
    if ((eal_ & 7u) != 0) stop(site, opcode, head, "list address is not 8-byte aligned");
    if ((size_ & 7u) != 0 || size_ > mfc::kMaxTransfer)
        stop(site, opcode, head, "list size is not a multiple of 8 up to 0x4000");
    if (tag_ >= mfc::kTagCount) stop(site, opcode, head, "tag id above 31");

    // The first element starts at MFC_LSA. Later ones start where the previous one ended, rounded up to 16
    // bytes; an element under 16 bytes takes the low four bits of its effective address, which the
    // alignment rule requires to match.
    std::uint32_t next_lsa = lsa_;
    for (std::uint32_t off = 0; off < size_; off += 8) {
        const std::uint32_t word0 = loadBe32(thread_.ls(), eal_ + off);
        const std::uint32_t ea = loadBe32(thread_.ls(), eal_ + off + 4);
        const std::uint32_t size = word0 & 0xffffu;
        std::uint32_t lsa = next_lsa;
        if (off != 0 && size < 16) lsa = (lsa & ~15u) | (ea & 15u);
        const Params element{lsa, ea, size, tag_};
        if ((word0 & kListStall) != 0) {
            stop(site, opcode, element, "list element with stall-and-notify is not implemented");
        }
        if (size != 0) transfer(to_ls, site, opcode, element);
        next_lsa = ((lsa & ~15u) + ((size + 15u) & ~15u)) & kLsMask;
    }
    bump(to_ls ? counters_.get_list : counters_.put_list);
}

void SpuMfc::atomic(std::uint32_t site, std::uint32_t opcode) {
    const Params p{lsa_ & ~kLineMask & kLsMask, eal_ & ~kLineMask, size_, tag_};
    if (size_ != kReservationLineSize) stop(site, opcode, p, "size is not 128");
    if (tag_ >= mfc::kTagCount) stop(site, opcode, p, "tag id above 31");
    std::uint8_t* line = thread_.ls() + p.lsa;
    switch (opcode) {
        case mfc::kGetLLAR:
            reservations().getllar(base_, p.ea, line, reservation_);
            untagLs(thread_, p.lsa, kReservationLineSize);
            atomic_status_ = mfc::kGetLLARDone;
            atomic_status_pending_ = true;
            bump(counters_.getllar);
            break;
        case mfc::kPutLLC: {
            const bool ok = reservations().putllc(base_, p.ea, line, reservation_);
            atomic_status_ = ok ? mfc::kPutLLCSuccess : mfc::kPutLLCFailed;
            atomic_status_pending_ = true;
            bump(ok ? counters_.putllc_ok : counters_.putllc_failed);
            break;
        }
        case mfc::kPutLLUC:
        case mfc::kPutQLLUC:
            reservations().putlluc(base_, p.ea, line);
            // The queued form reports no status.
            if (opcode == mfc::kPutLLUC) {
                atomic_status_ = mfc::kPutLLUCDone;
                atomic_status_pending_ = true;
            }
            bump(counters_.putlluc);
            break;
        default: break;
    }
}

void SpuMfc::command(std::uint32_t site, std::uint32_t value) {
    const std::uint32_t opcode = value & 0xffffu;
    const Params p{lsa_, eal_, size_, tag_};
    if (base_ == nullptr) stop(site, opcode, p, "no guest memory is attached");
    if (eah_ != 0 && opcode != mfc::kBarrier && opcode != mfc::kMfcEieio && opcode != mfc::kMfcSync) {
        stop(site, opcode, p, "effective address above 4 GiB");
    }
    switch (opcode) {
        case mfc::kGet:
        case mfc::kGetB:
        case mfc::kGetF: transfer(true, site, opcode, p); break;
        case mfc::kPut:
        case mfc::kPutB:
        case mfc::kPutF: transfer(false, site, opcode, p); break;
        case mfc::kGetL:
        case mfc::kGetLB:
        case mfc::kGetLF: list(true, site, opcode); break;
        case mfc::kPutL:
        case mfc::kPutLB:
        case mfc::kPutLF: list(false, site, opcode); break;
        case mfc::kGetLLAR:
        case mfc::kPutLLC:
        case mfc::kPutLLUC:
        case mfc::kPutQLLUC: atomic(site, opcode); break;
        case mfc::kBarrier:
        case mfc::kMfcEieio:
        case mfc::kMfcSync:
            // Transfers are complete and ordered already; the fence orders them against other host threads.
            std::atomic_thread_fence(std::memory_order_seq_cst);
            bump(counters_.fences);
            break;
        default: stop(site, opcode, p, "command is not implemented");
    }
}

void SpuMfc::writeTagUpdate(std::uint32_t site, std::uint32_t mode) {
    if (mode > 2) {
        spuStop(thread_,
                std::format("tox2: stop: spu: mfc tag update mode {} is invalid at ls {:#07x} on thread {:#010x}", mode,
                            site, thread_.id()));
    }
    tag_status_ = tag_mask_;
    tag_status_pending_ = true;
}

bool SpuMfc::takeTagStatus(std::uint32_t* value) {
    if (!tag_status_pending_) return false;
    *value = tag_status_;
    tag_status_pending_ = false;
    return true;
}

bool SpuMfc::takeAtomicStatus(std::uint32_t* value) {
    if (!atomic_status_pending_) return false;
    *value = atomic_status_;
    atomic_status_pending_ = false;
    return true;
}

}  // namespace tox2
