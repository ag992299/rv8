//
//  riscv-mmu.h
//

#ifndef riscv_mmu_h
#define riscv_mmu_h

namespace riscv {

	template <typename UX, typename TLB, typename PMA, typename MEMORY = user_memory<UX>>
	struct mmu
	{
		typedef TLB    tlb_type;
		typedef PMA    pma_type;
		typedef MEMORY memory_type;

		/* MMU properties */

		tlb_type       l1_dtlb;     /* L1 Data TLB */
		tlb_type       l1_itlb;     /* L1 Instruction TLB */
		pma_type       pma;         /* PMA table */
		memory_type    mem;         /* memory device */

		/* MMU methods */

		template <typename T> constexpr bool misaligned(UX va)
		{
			return (va & (sizeof(T) - 1)) != 0;
		}

		constexpr bool illegal(addr_t pa)
		{
			return pa == illegal_address;
		}

		// instruction fetch
		template <typename P> inst_t inst_fetch(P &proc, UX pc, addr_t &pc_offset)
		{
			typename tlb_type::tlb_entry_t* tlb_ent = nullptr;

			if (unlikely(misaligned<u16>(pc))) {
				proc.badaddr = pc;
				longjmp(proc.env, riscv_cause_misaligned_fetch);
			}

			addr_t mpa = translate_addr<P,false>(proc, pc, tlb_ent);
			addr_t uva = unlikely(illegal(mpa)) ? mpa : mem.mpa_to_uva(mpa);

			/* TODO: check tags, PMA, PTE, mode and alignment */

			inst_t inst;
			if (unlikely(illegal(uva))) {
				proc.badaddr = pc;
				longjmp(proc.env, riscv_cause_fault_fetch);
			} else {
				inst = riscv::inst_fetch(uva, pc_offset);
			}

			return inst;
		}

		// load
		template <typename P, typename T> void load(P &proc, UX va, T &val)
		{
			typename tlb_type::tlb_entry_t* tlb_ent = nullptr;

			if (unlikely(misaligned<T>(va))) {
				proc.badaddr = va;
				longjmp(proc.env, riscv_cause_misaligned_load);
			}

			addr_t mpa = translate_addr(proc, va, tlb_ent);
			addr_t uva = unlikely(illegal(mpa)) ? mpa : mem.mpa_to_uva(mpa);

			/* TODO: check tags, PMA, PTE, mode and alignment */

			if (unlikely(illegal(uva))) {
				proc.badaddr = va;
				longjmp(proc.env, riscv_cause_fault_load);
			} else {
				val = *static_cast<T*>((void*)uva);
			}
		}

		// store
		template <typename P, typename T> void store(P &proc, UX va, T val)
		{
			typename tlb_type::tlb_entry_t* tlb_ent = nullptr;

			if (unlikely(misaligned<T>(va))) {
				proc.badaddr = va;
				longjmp(proc.env, riscv_cause_misaligned_store);
			}

			addr_t mpa = translate_addr(proc, va, tlb_ent);
			addr_t uva = unlikely(illegal(mpa)) ? mpa : mem.mpa_to_uva(mpa);

			/* TODO: check tags, PMA, PTE, mode and alignment */

			if (unlikely(illegal(uva))) {
				proc.badaddr = va;
				longjmp(proc.env, riscv_cause_fault_store);
			} else {
				*static_cast<T*>((void*)uva) = val;
			}
		}

		// translate address based on processor translation mode
		template <typename P, bool inst_fetch = false> addr_t translate_addr(P &proc, UX va,
			typename tlb_type::tlb_entry_t* &tlb_ent)
		{
			addr_t pa = illegal_address;
			if (proc.mode == riscv_mode_M && proc.mstatus.r.mprv == 0) {
				pa = va;
			} else {
				switch (proc.mstatus.r.vm) {
					case riscv_vm_mbare:
						pa = va;
						break;
					case riscv_vm_sv32:
						pa = page_translate_addr<P,sv32>(proc, va, inst_fetch ? l1_itlb : l1_dtlb, tlb_ent);
						break;
					case riscv_vm_sv39:
						pa = page_translate_addr<P,sv39>(proc, va, inst_fetch ? l1_itlb : l1_dtlb, tlb_ent);
						break;
					case riscv_vm_sv48:
						pa = page_translate_addr<P,sv48>(proc, va, inst_fetch ? l1_itlb : l1_dtlb, tlb_ent);
						break;
				}
			}
			return pa;
		}

		// translate address using a TLB and a paged addressing mode
		template <typename P, typename PTM> addr_t page_translate_addr(
			P &proc, UX va,
			tlb_type &tlb, typename tlb_type::tlb_entry_t* &tlb_ent
		)
		{
			tlb_ent = tlb.lookup(proc.pdid, proc.sptbr >> tlb_type::ppn_bits, va);
			if (tlb_ent) {
				return (tlb_ent->ppn << page_shift) | (va & ~page_mask);
			} else {
				return page_translate_addr_tlb_miss<P,PTM>(proc, va, tlb, tlb_ent);
			}
		}

		// translate address using a TLB and a paged addressing mode
		// TLB miss slow path that invokes the page table walker
		template <typename P, typename PTM> addr_t page_translate_addr_tlb_miss(
			P &proc, UX va,
			tlb_type &tlb, typename tlb_type::tlb_entry_t* &tlb_ent
		)
		{
			/*
			 * The simple direct mapped TLB implementation currently maps page_size
			 * entries so as a byproduct, the code will rewalk the page table every
			 * page_size interval, even if the PTE points to a megapage or gigapage.
			 * Can be solved by adding a secondary TLB with larger scoped entries.
			 */

			typename PTM::pte_type pte;

			/* TODO: TLB statistics */

			addr_t pa;
			if ((pa = walk_page_table<P,PTM>(proc, va, tlb, tlb_ent, pte)) != illegal_address)
			{
				/* Insert the virtual to physical mapping into the TLB */
				tlb_ent = tlb.insert(proc.pdid, proc.sptbr >> tlb_type::ppn_bits,
					va, pte.val.flags, pte.val.ppn);
			}
			return pa;
		}

		template <typename P, typename PTM> addr_t walk_page_table(
			P &proc, UX va,
			tlb_type &tlb, typename tlb_type::tlb_entry_t* &tlb_ent,
			typename PTM::pte_type &pte
		)
		{
			typedef typename PTM::pte_type pte_type;

			UX ppn = proc.sptbr & ((1ULL << tlb_type::ppn_bits) - 1);
			UX vpn, pte_mpa;
			addr_t pte_uva;
			int shift, level;

			/* walk the page table */
			for (level = PTM::levels - 1; level >= 0; level--) {

				/* calculate the shift for this page table level */
				shift = PTM::bits * level + page_shift;
				vpn = (va >> shift) & ((1ULL << PTM::bits) - 1);
				pte_mpa = ppn + vpn * sizeof(pte_type);

				/* map the ppn into the host address space */
				pte_uva = mem.mpa_to_uva(pte_mpa);
				if (pte_uva == illegal_address) goto out;
				pte = *(pte_type*)pte_uva;

				/* If pte.v = 0, or if pte.r = 0 and pte.w = 1, raise an access exception */
				if (((~pte.val.flags >> pte_shift_V) |
					((~pte.val.flags >> pte_shift_R) & (pte.val.flags >> pte_shift_W))) & 1) goto out;

				/* translate address if we have a valid PTE */
				if ((pte.val.flags & (pte_flag_R | pte_flag_X))) {

					/* Construct address (could be a megapage or gigapage translation) */
					addr_t pa = (pte.val.ppn << page_shift) + (va & ((1ULL << shift) - 1));

					return pa; /* translated physical address */
				}

				/* step to the next entry */
				ppn = pte.val.ppn;

				/* clearing the pte holder so translation fault messages contain zeros */
				pte.xu.val = 0;
			}

		out:
			debug("walk_page_table va=%llx sptbr=%llx, level=%d ppn=%llx vpn=%llx pte=%llx: translation fault",
				(addr_t)va, (addr_t)proc.sptbr, level, (addr_t)ppn, (addr_t)vpn, (addr_t)pte.xu.val);

			return illegal_address;
		}
	};

	typedef tagged_tlb_rv32<128> tlb_type_rv32;
	typedef tagged_tlb_rv64<128> tlb_type_rv64;

	typedef pma_table<u32,8> pma_table_rv32;
	typedef pma_table<u64,8> pma_table_rv64;

	using mmu_rv32 = mmu<u32,tlb_type_rv32,pma_table_rv32>;
	using mmu_rv64 = mmu<u64,tlb_type_rv64,pma_table_rv64>;

}

#endif