﻿#include "stdafx.h"
#include "vm_locking.h"
#include "vm_ptr.h"
#include "vm_ref.h"
#include "vm_reservation.h"

#include "Utilities/mutex.h"
#include "Utilities/Thread.h"
#include "Utilities/address_range.h"
#include "Utilities/JIT.h"
#include "Emu/CPU/CPUThread.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/Cell/SPURecompiler.h"
#include "Emu/perf_meter.hpp"
#include <deque>

#include "util/vm.hpp"
#include "util/asm.hpp"

LOG_CHANNEL(vm_log, "VM");

void ppu_remove_hle_instructions(u32 addr, u32 size);

namespace vm
{
	static u8* memory_reserve_4GiB(void* _addr, u64 size = 0x100000000)
	{
		for (u64 addr = reinterpret_cast<u64>(_addr) + 0x100000000; addr < 0x8000'0000'0000; addr += 0x100000000)
		{
			if (auto ptr = utils::memory_reserve(size, reinterpret_cast<void*>(addr)))
			{
				return static_cast<u8*>(ptr);
			}
		}

		fmt::throw_exception("Failed to reserve vm memory");
	}

	// Emulated virtual memory
	u8* const g_base_addr = memory_reserve_4GiB(reinterpret_cast<void*>(0x2'0000'0000), 0x2'0000'0000);

	// Unprotected virtual memory mirror
	u8* const g_sudo_addr = g_base_addr + 0x1'0000'0000;

	// Auxiliary virtual memory for executable areas
	u8* const g_exec_addr = memory_reserve_4GiB(g_sudo_addr, 0x200000000);

	// Hooks for memory R/W interception (default: zero offset to some function with only ret instructions)
	u8* const g_hook_addr = memory_reserve_4GiB(g_exec_addr, 0x800000000);

	// Stats for debugging
	u8* const g_stat_addr = memory_reserve_4GiB(g_hook_addr);

	// For SPU
	u8* const g_free_addr = g_stat_addr + 0x1'0000'0000;

	// Reservation stats
	alignas(4096) u8 g_reservations[65536 / 128 * 64]{0};

	// Pointers to shared memory mirror or zeros for "normal" memory
	alignas(4096) atomic_t<u64> g_shmem[65536]{0};

	// Memory locations
	alignas(64) std::vector<std::shared_ptr<block_t>> g_locations;

	// Memory mutex acknowledgement
	thread_local atomic_t<cpu_thread*>* g_tls_locked = nullptr;

	// "Unique locked" range lock, as opposed to "shared" range locks from set
	atomic_t<u64> g_range_lock = 0;

	// Memory mutex: passive locks
	std::array<atomic_t<cpu_thread*>, g_cfg.core.ppu_threads.max> g_locks{};

	// Range lock slot allocation bits
	atomic_t<u64> g_range_lock_bits{};

	// Memory range lock slots (sparse atomics)
	atomic_t<u64, 64> g_range_lock_set[64]{};

	// Memory pages
	std::array<memory_page, 0x100000000 / 4096> g_pages;

	std::pair<bool, u64> try_reservation_update(u32 addr)
	{
		// Update reservation info with new timestamp
		auto& res = reservation_acquire(addr);
		const u64 rtime = res;

		return {!(rtime & vm::rsrv_unique_lock) && res.compare_and_swap_test(rtime, rtime + 128), rtime};
	}

	void reservation_update(u32 addr)
	{
		u64 old = -1;
		const auto cpu = get_current_cpu_thread();

		while (true)
		{
			const auto [ok, rtime] = try_reservation_update(addr);

			if (ok || (old & -128) < (rtime & -128))
			{
				if (ok)
				{
					reservation_notifier(addr).notify_all();
				}

				return;
			}

			old = rtime;

			if (cpu && cpu->test_stopped())
			{
				return;
			}
		}
	}

	static void _register_lock(cpu_thread* _cpu)
	{
		for (u32 i = 0, max = g_cfg.core.ppu_threads;;)
		{
			if (!g_locks[i] && g_locks[i].compare_and_swap_test(nullptr, _cpu))
			{
				g_tls_locked = g_locks.data() + i;
				break;
			}

			if (++i == max) i = 0;
		}
	}

	atomic_t<u64, 64>* alloc_range_lock()
	{
		const auto [bits, ok] = g_range_lock_bits.fetch_op([](u64& bits)
		{
			if (~bits) [[likely]]
			{
				bits |= bits + 1;
				return true;
			}

			return false;
		});

		if (!ok) [[unlikely]]
		{
			fmt::throw_exception("Out of range lock bits");
		}

		return &g_range_lock_set[std::countr_one(bits)];
	}

	void range_lock_internal(atomic_t<u64, 64>* range_lock, u32 begin, u32 size)
	{
		perf_meter<"RHW_LOCK"_u64> perf0;

		auto _cpu = get_current_cpu_thread();

		if (_cpu)
		{
			_cpu->state += cpu_flag::wait + cpu_flag::temp;
		}

		for (u64 i = 0;; i++)
		{
			range_lock->store(begin | (u64{size} << 32));

			const u64 lock_val = g_range_lock.load();
			const u64 is_share = g_shmem[begin >> 16].load();

			u64 lock_addr = static_cast<u32>(lock_val); // -> u64
			u32 lock_size = static_cast<u32>(lock_val << range_bits >> (range_bits + 32));

			u64 addr = begin;

			if ((lock_val & range_full_mask) == range_locked) [[likely]]
			{
				lock_size = 128;

				if (is_share)
				{
					addr = static_cast<u16>(addr) | is_share;
					lock_addr = lock_val;
				}
			}

			if (addr + size <= lock_addr || addr >= lock_addr + lock_size) [[likely]]
			{
				const u64 new_lock_val = g_range_lock.load();

				if (vm::check_addr(begin, vm::page_readable, size) && (!new_lock_val || new_lock_val == lock_val)) [[likely]]
				{
					break;
				}

				u32 test = 0;

				for (u32 i = begin / 4096, max = (begin + size - 1) / 4096; i <= max; i++)
				{
					if (!(g_pages[i] & (vm::page_readable)))
					{
						test = i * 4096;
						break;
					}
				}

				if (test)
				{
					range_lock->release(0);

					// Try triggering a page fault (write)
					// TODO: Read memory if needed
					vm::_ref<atomic_t<u8>>(test) += 0;
					continue;
				}
			}

			// Wait a bit before accessing global lock
			range_lock->store(0);
			busy_wait(200);
		}

		if (_cpu)
		{
			_cpu->check_state();
		}
	}

	void free_range_lock(atomic_t<u64, 64>* range_lock) noexcept
	{
		if (range_lock < g_range_lock_set || range_lock >= std::end(g_range_lock_set))
		{
			fmt::throw_exception("Invalid range lock");
		}

		range_lock->release(0);

		// Use ptr difference to determine location
		const auto diff = range_lock - g_range_lock_set;
		g_range_lock_bits &= ~(1ull << diff);
	}

	template <typename F>
	FORCE_INLINE static u64 for_all_range_locks(u64 input, F func)
	{
		u64 result = input;

		for (u64 bits = input; bits; bits &= bits - 1)
		{
			const u32 id = std::countr_zero(bits);

			const u64 lock_val = g_range_lock_set[id].load();

			if (const u32 size = static_cast<u32>(lock_val >> 32)) [[unlikely]]
			{
				const u32 addr = static_cast<u32>(lock_val);

				if (func(addr, size)) [[unlikely]]
				{
					continue;
				}
			}

			result &= ~(1ull << id);
		}

		return result;
	}

	static void _lock_main_range_lock(u64 flags, u32 addr, u32 size)
	{
		// Shouldn't really happen
		if (size == 0)
		{
			vm_log.warning("Tried to lock empty range (flags=0x%x, addr=0x%x)", flags >> 32, addr);
			return;
		}

		// Limit to <512 MiB at once; make sure if it operates on big amount of data, it's page-aligned
		if (size >= 512 * 1024 * 1024 || (size > 65536 && size % 4096))
		{
			fmt::throw_exception("Failed to lock range (flags=0x%x, addr=0x%x, size=0x%x)", flags >> 32, addr, size);
		}

		// Block or signal new range locks
		g_range_lock = addr | u64{size} << 32 | flags;

		utils::prefetch_read(g_range_lock_set + 0);
		utils::prefetch_read(g_range_lock_set + 2);
		utils::prefetch_read(g_range_lock_set + 4);

		const auto range = utils::address_range::start_length(addr, size);

		u64 to_clear = g_range_lock_bits.load();

		while (to_clear)
		{
			to_clear = for_all_range_locks(to_clear, [&](u32 addr2, u32 size2)
			{
				if (range.overlaps(utils::address_range::start_length(addr2, size2))) [[unlikely]]
				{
					return 1;
				}

				return 0;
			});

			if (!to_clear) [[likely]]
			{
				break;
			}

			utils::pause();
		}
	}

	void passive_lock(cpu_thread& cpu)
	{
		bool ok = true;

		if (!g_tls_locked || *g_tls_locked != &cpu) [[unlikely]]
		{
			_register_lock(&cpu);

			if (cpu.state & cpu_flag::memory) [[likely]]
			{
				cpu.state -= cpu_flag::memory;
			}

			if (!g_range_lock)
			{
				return;
			}

			ok = false;
		}

		if (!ok || cpu.state & cpu_flag::memory)
		{
			for (u64 i = 0;; i++)
			{
				if (i < 100)
					busy_wait(200);
				else
					std::this_thread::yield();

				if (g_range_lock)
				{
					continue;
				}

				cpu.state -= cpu_flag::memory;

				if (!g_range_lock) [[likely]]
				{
					return;
				}
			}
		}
	}

	void passive_unlock(cpu_thread& cpu)
	{
		if (auto& ptr = g_tls_locked)
		{
			ptr->release(nullptr);
			ptr = nullptr;

			if (cpu.state & cpu_flag::memory)
			{
				cpu.state -= cpu_flag::memory;
			}
		}
	}

	void temporary_unlock(cpu_thread& cpu) noexcept
	{
		if (!(cpu.state & cpu_flag::wait)) cpu.state += cpu_flag::wait;

		if (g_tls_locked && g_tls_locked->compare_and_swap_test(&cpu, nullptr))
		{
			cpu.state += cpu_flag::memory;
		}
	}

	void temporary_unlock() noexcept
	{
		if (auto cpu = get_current_cpu_thread())
		{
			temporary_unlock(*cpu);
		}
	}

	writer_lock::writer_lock()
		: writer_lock(0, 1)
	{
	}

	writer_lock::writer_lock(u32 const addr, u32 const size, u64 const flags)
	{
		auto cpu = get_current_cpu_thread();

		if (cpu)
		{
			if (!g_tls_locked || *g_tls_locked != cpu || cpu->state & cpu_flag::wait)
			{
				cpu = nullptr;
			}
			else
			{
				cpu->state += cpu_flag::wait;
			}
		}

		for (u64 i = 0;; i++)
		{
			if (g_range_lock || !g_range_lock.compare_and_swap_test(0, addr | u64{size} << 32 | flags))
			{
				if (i < 100)
					busy_wait(200);
				else
					std::this_thread::yield();
			}
			else
			{
				break;
			}
		}

		if (addr >= 0x10000)
		{
			perf_meter<"SUSPEND"_u64> perf0;

			for (auto lock = g_locks.cbegin(), end = lock + g_cfg.core.ppu_threads; lock != end; lock++)
			{
				if (auto ptr = +*lock; ptr && !(ptr->state & cpu_flag::memory))
				{
					ptr->state.test_and_set(cpu_flag::memory);
				}
			}

			u64 addr1 = addr;

			if (u64 is_shared = g_shmem[addr >> 16]) [[unlikely]]
			{
				// Reservation address in shareable memory range
				addr1 = static_cast<u16>(addr) | is_shared;
			}

			utils::prefetch_read(g_range_lock_set + 0);
			utils::prefetch_read(g_range_lock_set + 2);
			utils::prefetch_read(g_range_lock_set + 4);

			u64 to_clear = g_range_lock_bits.load();

			u64 point = addr1 / 128;

			while (true)
			{
				to_clear = for_all_range_locks(to_clear, [&](u64 addr2, u32 size2)
				{
					// TODO (currently not possible): handle 2 64K pages (inverse range), or more pages
					if (u64 is_shared = g_shmem[addr2 >> 16]) [[unlikely]]
					{
						addr2 = static_cast<u16>(addr2) | is_shared;
					}

					if (point - (addr2 / 128) <= (addr2 + size2 - 1) / 128 - (addr2 / 128)) [[unlikely]]
					{
						return 1;
					}

					return 0;
				});

				if (!to_clear) [[likely]]
				{
					break;
				}

				utils::pause();
			}

			for (auto lock = g_locks.cbegin(), end = lock + g_cfg.core.ppu_threads; lock != end; lock++)
			{
				if (auto ptr = +*lock)
				{
					while (!(ptr->state & cpu_flag::wait))
					{
						utils::pause();
					}
				}
			}
		}

		if (cpu)
		{
			cpu->state -= cpu_flag::memory + cpu_flag::wait;
		}
	}

	writer_lock::~writer_lock()
	{
		g_range_lock = 0;
	}

	u64 reservation_lock_internal(u32 addr, atomic_t<u64>& res)
	{
		for (u64 i = 0;; i++)
		{
			if (u64 rtime = res; !(rtime & 127) && reservation_try_lock(res, rtime)) [[likely]]
			{
				return rtime;
			}

			if (auto cpu = get_current_cpu_thread(); cpu && cpu->state)
			{
				cpu->check_state();
			}
			else if (i < 15)
			{
				busy_wait(500);
			}
			else
			{
				// TODO: Accurate locking in this case
				if (!(g_pages[addr / 4096] & page_writable))
				{
					return -1;
				}

				std::this_thread::yield();
			}
		}
	}

	void reservation_shared_lock_internal(atomic_t<u64>& res)
	{
		for (u64 i = 0;; i++)
		{
			auto [_oldd, _ok] = res.fetch_op([&](u64& r)
			{
				if (r & rsrv_unique_lock)
				{
					return false;
				}

				r += 1;
				return true;
			});

			if (_ok) [[likely]]
			{
				return;
			}

			if (auto cpu = get_current_cpu_thread(); cpu && cpu->state)
			{
				cpu->check_state();
			}
			else if (i < 15)
			{
				busy_wait(500);
			}
			else
			{
				std::this_thread::yield();
			}
		}
	}

	void reservation_op_internal(u32 addr, std::function<bool()> func)
	{
		auto& res = vm::reservation_acquire(addr);
		auto* ptr = vm::get_super_ptr(addr & -128);

		cpu_thread::suspend_all<+1>(get_current_cpu_thread(), {ptr, ptr + 64, &res}, [&]
		{
			if (func())
			{
				// Success, release the lock and progress
				res += 127;
			}
			else
			{
				// Only release the lock on failure
				res -= 1;
			}
		});
	}

	[[noreturn]] void reservation_escape_internal()
	{
		const auto _cpu = get_current_cpu_thread();

		if (_cpu && _cpu->id_type() == 1)
		{
			// TODO: PPU g_escape
		}

		if (_cpu && _cpu->id_type() == 2)
		{
			spu_runtime::g_escape(static_cast<spu_thread*>(_cpu));
		}

		thread_ctrl::emergency_exit("vm::reservation_escape");
	}

	static void _page_map(u32 addr, u8 flags, u32 size, utils::shm* shm, u64 bflags, std::pair<const u32, std::pair<u32, std::shared_ptr<utils::shm>>>* (*search_shm)(vm::block_t* block, utils::shm* shm))
	{
		perf_meter<"PAGE_MAP"_u64> perf0;

		if (!size || (size | addr) % 4096 || flags & page_allocated)
		{
			fmt::throw_exception("Invalid arguments (addr=0x%x, size=0x%x)", addr, size);
		}

		for (u32 i = addr / 4096; i < addr / 4096 + size / 4096; i++)
		{
			if (g_pages[i])
			{
				fmt::throw_exception("Memory already mapped (addr=0x%x, size=0x%x, flags=0x%x, current_addr=0x%x)", addr, size, flags, i * 4096);
			}
		}

		// If native page size exceeds 4096, don't map native pages (expected to be always mapped in this case)
		const bool is_noop = bflags & page_size_4k && utils::c_page_size > 4096;

		// Lock range being mapped
		_lock_main_range_lock(range_allocation, addr, size);

		if (shm && shm->flags() != 0 && shm->info++)
		{
			// Check ref counter (using unused member info for it)
			if (shm->info == 2)
			{
				// Allocate shm object for itself
				u64 shm_self = reinterpret_cast<u64>(shm->map_self()) ^ range_locked;

				// Pre-set range-locked flag (real pointers are 47 bits)
				// 1. To simplify range_lock logic
				// 2. To make sure it never overlaps with 32-bit addresses
				// Also check that it's aligned (lowest 16 bits)
				ensure((shm_self & 0xffff'0000'0000'ffff) == range_locked);

				// Find another mirror and map it as shareable too
				for (auto& ploc : g_locations)
				{
					if (auto loc = ploc.get())
					{
						if (auto pp = search_shm(loc, shm))
						{
							auto& [size2, ptr] = pp->second;

							for (u32 i = pp->first / 65536; i < pp->first / 65536 + size2 / 65536; i++)
							{
								g_shmem[i].release(shm_self);

								// Advance to the next position
								shm_self += 0x10000;
							}
						}
					}
				}

				// Unsharing only happens on deallocation currently, so make sure all further refs are shared
				shm->info = 0xffff'ffff;
			}

			// Obtain existing pointer
			u64 shm_self = reinterpret_cast<u64>(shm->get()) ^ range_locked;

			// Check (see above)
			ensure((shm_self & 0xffff'0000'0000'ffff) == range_locked);

			// Map range as shareable
			for (u32 i = addr / 65536; i < addr / 65536 + size / 65536; i++)
			{
				g_shmem[i].release(std::exchange(shm_self, shm_self + 0x10000));
			}
		}

		// Notify rsx that range has become valid
		// Note: This must be done *before* memory gets mapped while holding the vm lock, otherwise
		//       the RSX might try to invalidate memory that got unmapped and remapped
		if (const auto rsxthr = g_fxo->try_get<rsx::thread>())
		{
			rsxthr->on_notify_memory_mapped(addr, size);
		}

		auto prot = utils::protection::rw;
		if (~flags & page_writable)
			prot = utils::protection::ro;
		if (~flags & page_readable)
			prot = utils::protection::no;

		if (is_noop)
		{
		}
		else if (!shm)
		{
			utils::memory_protect(g_base_addr + addr, size, prot);
		}
		else if (shm->map_critical(g_base_addr + addr, prot) != g_base_addr + addr || shm->map_critical(g_sudo_addr + addr) != g_sudo_addr + addr || !shm->map_self())
		{
			fmt::throw_exception("Memory mapping failed - blame Windows (addr=0x%x, size=0x%x, flags=0x%x)", addr, size, flags);
		}

		if (flags & page_executable && !is_noop)
		{
			// TODO (dead code)
			utils::memory_commit(g_exec_addr + addr * 2, size * 2);

			if (g_cfg.core.ppu_debug)
			{
				utils::memory_commit(g_stat_addr + addr, size);
			}
		}

		for (u32 i = addr / 4096; i < addr / 4096 + size / 4096; i++)
		{
			if (g_pages[i].exchange(flags | page_allocated))
			{
				fmt::throw_exception("Concurrent access (addr=0x%x, size=0x%x, flags=0x%x, current_addr=0x%x)", addr, size, flags, i * 4096);
			}
		}
	}

	bool page_protect(u32 addr, u32 size, u8 flags_test, u8 flags_set, u8 flags_clear)
	{
		perf_meter<"PAGE_PRO"_u64> perf0;

		vm::writer_lock lock;

		if (!size || (size | addr) % 4096)
		{
			fmt::throw_exception("Invalid arguments (addr=0x%x, size=0x%x)", addr, size);
		}

		const u8 flags_both = flags_set & flags_clear;

		flags_test  |= page_allocated;
		flags_set   &= ~flags_both;
		flags_clear &= ~flags_both;

		if (!check_addr(addr, flags_test, size))
		{
			return false;
		}

		if (!flags_set && !flags_clear)
		{
			return true;
		}

		// Choose some impossible value (not valid without page_allocated)
		u8 start_value = page_executable;

		for (u32 start = addr / 4096, end = start + size / 4096, i = start; i < end + 1; i++)
		{
			u8 new_val = page_executable;

			if (i < end)
			{
				new_val = g_pages[i];
				new_val |= flags_set;
				new_val &= ~flags_clear;
			}

			if (new_val != start_value)
			{
				const u8 old_val = g_pages[start];

				if (u32 page_size = (i - start) * 4096; page_size && old_val != start_value)
				{
					u64 safe_bits = 0;

					if (old_val & start_value & page_readable)
						safe_bits |= range_readable;
					if (old_val & start_value & page_writable && safe_bits & range_readable)
						safe_bits |= range_writable;

					// Protect range locks from observing changes in memory protection
					_lock_main_range_lock(safe_bits, start * 4096, page_size);

					for (u32 j = start; j < i; j++)
					{
						g_pages[j].release(start_value);
					}

					if ((old_val ^ start_value) & (page_readable | page_writable))
					{
						const auto protection = start_value & page_writable ? utils::protection::rw : (start_value & page_readable ? utils::protection::ro : utils::protection::no);
						utils::memory_protect(g_base_addr + start * 4096, page_size, protection);
					}
				}

				start_value = new_val;
				start = i;
			}
		}

		return true;
	}

	static u32 _page_unmap(u32 addr, u32 max_size, u64 bflags, utils::shm* shm)
	{
		perf_meter<"PAGE_UNm"_u64> perf0;

		if (!max_size || (max_size | addr) % 4096)
		{
			fmt::throw_exception("Invalid arguments (addr=0x%x, max_size=0x%x)", addr, max_size);
		}

		// If native page size exceeds 4096, don't unmap native pages (always mapped)
		const bool is_noop = bflags & page_size_4k && utils::c_page_size > 4096;

		// Determine deallocation size
		u32 size = 0;
		bool is_exec = false;

		for (u32 i = addr / 4096; i < addr / 4096 + max_size / 4096; i++)
		{
			if ((g_pages[i] & page_allocated) == 0)
			{
				break;
			}

			if (size == 0)
			{
				is_exec = !!(g_pages[i] & page_executable);
			}
			else
			{
				// Must be consistent
				ensure(is_exec == !!(g_pages[i] & page_executable));
			}

			size += 4096;
		}

		// Protect range locks from actual memory protection changes
		_lock_main_range_lock(range_allocation, addr, size);

		if (shm && shm->flags() != 0 && g_shmem[addr >> 16])
		{
			shm->info--;

			for (u32 i = addr / 65536; i < addr / 65536 + size / 65536; i++)
			{
				g_shmem[i].release(0);
			}
		}

		for (u32 i = addr / 4096; i < addr / 4096 + size / 4096; i++)
		{
			if (!(g_pages[i] & page_allocated))
			{
				fmt::throw_exception("Concurrent access (addr=0x%x, size=0x%x, current_addr=0x%x)", addr, size, i * 4096);
			}

			g_pages[i].release(0);
		}

		// Notify rsx to invalidate range
		// Note: This must be done *before* memory gets unmapped while holding the vm lock, otherwise
		//       the RSX might try to call VirtualProtect on memory that is already unmapped
		if (auto& rsxthr = g_fxo->get<rsx::thread>(); g_fxo->is_init<rsx::thread>())
		{
			rsxthr.on_notify_memory_unmapped(addr, size);
		}

		// Deregister PPU related data
		ppu_remove_hle_instructions(addr, size);

		// Actually unmap memory
		if (is_noop)
		{
			std::memset(g_sudo_addr + addr, 0, size);
		}
		else if (!shm)
		{
			utils::memory_protect(g_base_addr + addr, size, utils::protection::no);
			std::memset(g_sudo_addr + addr, 0, size);
		}
		else
		{
			shm->unmap_critical(g_base_addr + addr);
#ifdef _WIN32
			shm->unmap_critical(g_sudo_addr + addr);
#endif
		}

		if (is_exec && !is_noop)
		{
			utils::memory_decommit(g_exec_addr + addr * 2, size * 2);

			if (g_cfg.core.ppu_debug)
			{
				utils::memory_decommit(g_stat_addr + addr, size);
			}
		}

		return size;
	}

	bool check_addr(u32 addr, u8 flags, u32 size)
	{
		if (size == 0)
		{
			return true;
		}

		// Overflow checking
		if (0x10000'0000ull - addr < size)
		{
			return false;
		}

		// Always check this flag
		flags |= page_allocated;

		for (u32 i = addr / 4096, max = (addr + size - 1) / 4096; i <= max;)
		{
			auto state = +g_pages[i];

			if (~state & flags) [[unlikely]]
			{
				return false;
			}

			if (state & page_1m_size)
			{
				i = utils::align(i + 1, 0x100000 / 4096);
				continue;
			}

			if (state & page_64k_size)
			{
				i = utils::align(i + 1, 0x10000 / 4096);
				continue;
			}

			i++;
		}

		return true;
	}

	u32 alloc(u32 size, memory_location_t location, u32 align)
	{
		const auto block = get(location);

		if (!block)
		{
			vm_log.error("vm::alloc(): Invalid memory location (%u)", +location);
			ensure(location < memory_location_max); // The only allowed locations to fail
			return 0;
		}

		return block->alloc(size, nullptr, align);
	}

	u32 falloc(u32 addr, u32 size, memory_location_t location, const std::shared_ptr<utils::shm>* src)
	{
		const auto block = get(location, addr);

		if (!block)
		{
			vm_log.error("vm::falloc(): Invalid memory location (%u, addr=0x%x)", +location, addr);
			ensure(location == any || location < memory_location_max); // The only allowed locations to fail
			return 0;
		}

		return block->falloc(addr, size, src);
	}

	u32 dealloc(u32 addr, memory_location_t location, const std::shared_ptr<utils::shm>* src)
	{
		const auto block = get(location, addr);

		if (!block)
		{
			vm_log.error("vm::dealloc(): Invalid memory location (%u, addr=0x%x)", +location, addr);
			ensure(location == any || location < memory_location_max); // The only allowed locations to fail
			return 0;
		}

		return block->dealloc(addr, src);
	}

	void lock_sudo(u32 addr, u32 size)
	{
		perf_meter<"PAGE_LCK"_u64> perf;

		ensure(addr % 4096 == 0);
		ensure(size % 4096 == 0);

		if (!utils::memory_lock(g_sudo_addr + addr, size))
		{
			vm_log.error("Failed to lock sudo memory (addr=0x%x, size=0x%x). Consider increasing your system limits.", addr, size);
		}
	}

	// Mapped regions: addr -> shm handle
	constexpr auto block_map = &auto_typemap<block_t>::get<std::map<u32, std::pair<u32, std::shared_ptr<utils::shm>>>>;

	bool block_t::try_alloc(u32 addr, u64 bflags, u32 size, std::shared_ptr<utils::shm>&& shm) const
	{
		// Check if memory area is already mapped
		for (u32 i = addr / 4096; i <= (addr + size - 1) / 4096; i++)
		{
			if (g_pages[i])
			{
				return false;
			}
		}

		const u32 page_addr = addr + (this->flags & stack_guarded ? 0x1000 : 0);
		const u32 page_size = size - (this->flags & stack_guarded ? 0x2000 : 0);

		// No flags are default to readable/writable
		// Explicit (un...) flags are used to protect from such access
		u8 flags = 0;

		if (~bflags & alloc_hidden)
		{
			flags |= page_readable;

			if (~bflags & alloc_unwritable)
			{
				flags |= page_writable;
			}
		}

		if (bflags & alloc_executable)
		{
			flags |= page_executable;
		}

		if ((bflags & page_size_mask) == page_size_64k)
		{
			flags |= page_64k_size;
		}
		else if (!(bflags & (page_size_mask & ~page_size_1m)))
		{
			flags |= page_1m_size;
		}

		if (this->flags & stack_guarded)
		{
			// Mark overflow/underflow guard pages as allocated
			ensure(!g_pages[addr / 4096].exchange(page_allocated));
			ensure(!g_pages[addr / 4096 + size / 4096 - 1].exchange(page_allocated));
		}

		// Map "real" memory pages; provide a function to search for mirrors with private member access
		_page_map(page_addr, flags, page_size, shm.get(), this->flags, [](vm::block_t* _this, utils::shm* shm)
		{
			auto& map = (_this->m.*block_map)();

			std::remove_reference_t<decltype(map)>::value_type* result = nullptr;

			// Check eligibility
			if (!_this || !(page_size_mask & _this->flags) || _this->addr < 0x20000000 || _this->addr >= 0xC0000000)
			{
				return result;
			}

			for (auto& pp : map)
			{
				if (pp.second.second.get() == shm)
				{
					// Found match
					return &pp;
				}
			}

			return result;
		});

		// Fill stack guards with STACKGRD
		if (this->flags & stack_guarded)
		{
			auto fill64 = [](u8* ptr, u64 data, usz count)
			{
#ifdef _M_X64
				__stosq(reinterpret_cast<u64*>(ptr), data, count);
#elif defined(ARCH_X64)
				__asm__ ("mov %0, %%rdi; mov %1, %%rax; mov %2, %%rcx; rep stosq;"
					:
					: "r" (ptr), "r" (data), "r" (count)
					: "rdi", "rax", "rcx", "memory");
#else
				for (usz i = 0; i < count; i++)
					reinterpret_cast<u64*>(ptr)[i] = data;
#endif
			};

			const u32 enda = addr + size - 4096;
			fill64(g_sudo_addr + addr, "STACKGRD"_u64, 4096 / sizeof(u64));
			fill64(g_sudo_addr + enda, "UNDERFLO"_u64, 4096 / sizeof(u64));
		}

		// Add entry
		(m.*block_map)()[addr] = std::make_pair(size, std::move(shm));

		return true;
	}

	static constexpr u64 process_block_flags(u64 flags)
	{
		if ((flags & page_size_mask) == 0)
		{
			flags |= page_size_1m;
		}

		if (flags & page_size_4k)
		{
			flags |= preallocated;
		}
		else
		{
			flags &= ~stack_guarded;
		}

		return flags;
	}

	block_t::block_t(u32 addr, u32 size, u64 flags)
		: m_id([](){ static atomic_t<u64> s_id = 1; return s_id++; }())
		, addr(addr)
		, size(size)
		, flags(process_block_flags(flags))
	{
		if (this->flags & preallocated)
		{
			// Special path for whole-allocated areas allowing 4k granularity
			m_common = std::make_shared<utils::shm>(size);
			m_common->map_critical(vm::base(addr), this->flags & page_size_4k && utils::c_page_size > 4096 ? utils::protection::rw : utils::protection::no);
			m_common->map_critical(vm::get_super_ptr(addr));
		}
	}

	bool block_t::unmap()
	{
		auto& m_map = (m.*block_map)();

		if (m_id.exchange(0))
		{
			// Deallocate all memory
			for (auto it = m_map.begin(), end = m_map.end(); it != end;)
			{
				const auto next = std::next(it);
				const auto size = it->second.first;
				_page_unmap(it->first, size, this->flags, it->second.second.get());
				it = next;
			}

			if (m_common)
			{
				m_common->unmap_critical(vm::base(addr));
#ifdef _WIN32
				m_common->unmap_critical(vm::get_super_ptr(addr));
#endif
			}

			return true;
		}

		return false;
	}

	block_t::~block_t()
	{
		ensure(!is_valid());
	}

	u32 block_t::alloc(const u32 orig_size, const std::shared_ptr<utils::shm>* src, u32 align, u64 flags)
	{
		if (!src)
		{
			// Use the block's flags (excpet for protection)
			flags = (this->flags & ~alloc_prot_mask) | (flags & alloc_prot_mask);
		}

		// Determine minimal alignment
		const u32 min_page_size = flags & page_size_4k ? 0x1000 : 0x10000;

		// Align to minimal page size
		const u32 size = utils::align(orig_size, min_page_size) + (flags & stack_guarded ? 0x2000 : 0);

		// Check alignment (it's page allocation, so passing small values there is just silly)
		if (align < min_page_size || align != (0x80000000u >> std::countl_zero(align)))
		{
			fmt::throw_exception("Invalid alignment (size=0x%x, align=0x%x)", size, align);
		}

		// Return if size is invalid
		if (!orig_size || !size || orig_size > size || size > this->size)
		{
			return 0;
		}

		// Create or import shared memory object
		std::shared_ptr<utils::shm> shm;

		if (m_common)
			ensure(!src);
		else if (src)
			shm = *src;
		else
		{
			shm = std::make_shared<utils::shm>(size);
		}

		const u32 max = (this->addr + this->size - size) & (0 - align);

		u32 addr = utils::align(this->addr, align);

		if (this->addr > std::min(max, addr))
		{
			return 0;
		}

		vm::writer_lock lock;

		if (!is_valid())
		{
			// Expired block
			return 0;
		}

		// Search for an appropriate place (unoptimized)
		for (;; addr += align)
		{
			if (try_alloc(addr, flags, size, std::move(shm)))
			{
				return addr + (flags & stack_guarded ? 0x1000 : 0);
			}

			if (addr == max)
			{
				break;
			}
		}

		return 0;
	}

	u32 block_t::falloc(u32 addr, const u32 orig_size, const std::shared_ptr<utils::shm>* src, u64 flags)
	{
		if (!src)
		{
			// Use the block's flags (excpet for protection)
			flags = (this->flags & ~alloc_prot_mask) | (flags & alloc_prot_mask);
		}

		// Determine minimal alignment
		const u32 min_page_size = flags & page_size_4k ? 0x1000 : 0x10000;

		// Take address misalignment into account
		const u32 size0 = orig_size + addr % min_page_size;

		// Align to minimal page size
		const u32 size = utils::align(size0, min_page_size);

		// Return if addr or size is invalid
		// If shared memory is provided, addr/size must be aligned
		if (!size ||
			addr < this->addr ||
			orig_size > size0 ||
			orig_size > size ||
			(addr - addr % min_page_size) + u64{size} > this->addr + u64{this->size} ||
			(src && (orig_size | addr) % min_page_size) ||
			flags & stack_guarded)
		{
			return 0;
		}

		// Force aligned address
		addr -= addr % min_page_size;

		// Create or import shared memory object
		std::shared_ptr<utils::shm> shm;

		if (m_common)
			ensure(!src);
		else if (src)
			shm = *src;
		else
		{
			shm = std::make_shared<utils::shm>(size);
		}

		vm::writer_lock lock;

		if (!is_valid())
		{
			// Expired block
			return 0;
		}

		if (!try_alloc(addr, flags, size, std::move(shm)))
		{
			return 0;
		}

		return addr;
	}

	u32 block_t::dealloc(u32 addr, const std::shared_ptr<utils::shm>* src) const
	{
		auto& m_map = (m.*block_map)();
		{
			vm::writer_lock lock;

			const auto found = m_map.find(addr - (flags & stack_guarded ? 0x1000 : 0));

			if (found == m_map.end())
			{
				return 0;
			}

			if (src && found->second.second.get() != src->get())
			{
				return 0;
			}

			// Get allocation size
			const auto size = found->second.first - (flags & stack_guarded ? 0x2000 : 0);

			if (flags & stack_guarded)
			{
				// Clear guard pages
				ensure(g_pages[addr / 4096 - 1].exchange(0) == page_allocated);
				ensure(g_pages[addr / 4096 + size / 4096].exchange(0) == page_allocated);
			}

			// Unmap "real" memory pages
			ensure(size == _page_unmap(addr, size, this->flags, found->second.second.get()));

			// Clear stack guards
			if (flags & stack_guarded)
			{
				std::memset(g_sudo_addr + addr - 4096, 0, 4096);
				std::memset(g_sudo_addr + addr + size, 0, 4096);
			}

			// Remove entry
			m_map.erase(found);

			return size;
		}
	}

	std::pair<u32, std::shared_ptr<utils::shm>> block_t::peek(u32 addr, u32 size) const
	{
		if (addr < this->addr || addr + u64{size} > this->addr + u64{this->size})
		{
			return {addr, nullptr};
		}

		auto& m_map = (m.*block_map)();

		vm::writer_lock lock;

		const auto upper = m_map.upper_bound(addr);

		if (upper == m_map.begin())
		{
			return {addr, nullptr};
		}

		const auto found = std::prev(upper);

		// Exact address condition (size == 0)
		if (size == 0 && found->first != addr)
		{
			return {addr, nullptr};
		}

		// Special case
		if (m_common)
		{
			return {addr, nullptr};
		}

		// Range check
		if (addr + u64{size} > found->first + u64{found->second.second->size()})
		{
			return {addr, nullptr};
		}

		return {found->first, found->second.second};
	}

	u32 block_t::imp_used(const vm::writer_lock&) const
	{
		u32 result = 0;

		for (auto& entry : (m.*block_map)())
		{
			result += entry.second.first - (flags & stack_guarded ? 0x2000 : 0);
		}

		return result;
	}

	u32 block_t::used()
	{
		vm::writer_lock lock;

		return imp_used(lock);
	}

	static bool _test_map(u32 addr, u32 size)
	{
		const auto range = utils::address_range::start_length(addr, size);

		if (!range.valid())
		{
			return false;
		}

		for (auto& block : g_locations)
		{
			if (!block)
			{
				continue;
			}

			if (range.overlaps(utils::address_range::start_length(block->addr, block->size)))
			{
				return false;
			}
		}

		return true;
	}

	static std::shared_ptr<block_t> _find_map(u32 size, u32 align, u64 flags)
	{
		const u32 max = (0xC0000000 - size) & (0 - align);

		if (size > 0xC0000000 - 0x20000000 || max < 0x20000000)
		{
			return nullptr;
		}

		for (u32 addr = utils::align<u32>(0x20000000, align);; addr += align)
		{
			if (_test_map(addr, size))
			{
				return std::make_shared<block_t>(addr, size, flags);
			}

			if (addr == max)
			{
				break;
			}
		}

		return nullptr;
	}

	static std::shared_ptr<block_t> _map(u32 addr, u32 size, u64 flags)
	{
		if (!size || (size | addr) % 4096)
		{
			fmt::throw_exception("Invalid arguments (addr=0x%x, size=0x%x)", addr, size);
		}

		if (!_test_map(addr, size))
		{
			return nullptr;
		}

		for (u32 i = addr / 4096; i < addr / 4096 + size / 4096; i++)
		{
			if (g_pages[i])
			{
				fmt::throw_exception("Unexpected pages allocated (current_addr=0x%x)", i * 4096);
			}
		}

		auto block = std::make_shared<block_t>(addr, size, flags);

		g_locations.emplace_back(block);

		return block;
	}

	static std::shared_ptr<block_t> _get_map(memory_location_t location, u32 addr)
	{
		if (location != any)
		{
			// return selected location
			if (location < g_locations.size())
			{
				return g_locations[location];
			}

			return nullptr;
		}

		// search location by address
		for (auto& block : g_locations)
		{
			if (block && addr >= block->addr && addr <= block->addr + block->size - 1)
			{
				return block;
			}
		}

		return nullptr;
	}

	std::shared_ptr<block_t> map(u32 addr, u32 size, u64 flags)
	{
		vm::writer_lock lock;

		return _map(addr, size, flags);
	}

	std::shared_ptr<block_t> find_map(u32 orig_size, u32 align, u64 flags)
	{
		vm::writer_lock lock;

		// Align to minimal page size
		const u32 size = utils::align(orig_size, 0x10000);

		// Check alignment
		if (align < 0x10000 || align != (0x80000000u >> std::countl_zero(align)))
		{
			fmt::throw_exception("Invalid alignment (size=0x%x, align=0x%x)", size, align);
		}

		// Return if size is invalid
		if (!size)
		{
			return nullptr;
		}

		auto block = _find_map(size, align, flags);

		if (block) g_locations.emplace_back(block);

		return block;
	}

	std::pair<std::shared_ptr<block_t>, bool> unmap(u32 addr, bool must_be_empty, const std::shared_ptr<block_t>* ptr)
	{
		if (ptr)
		{
			addr = (*ptr)->addr;
		}

		std::pair<std::shared_ptr<block_t>, bool> result{};

		vm::writer_lock lock;

		for (auto it = g_locations.begin() + memory_location_max; it != g_locations.end(); it++)
		{
			if (*it && (*it)->addr == addr)
			{
				if (must_be_empty && (*it)->flags & bf0_mask)
				{
					continue;
				}

				if (!must_be_empty && ((*it)->flags & bf0_mask) != bf0_0x2)
				{
					continue;
				}

				if (ptr && *it != *ptr)
				{
					return {};
				}

				if (must_be_empty && (*it)->imp_used(lock))
				{
					result.first = *it;
					return result;
				}

				result.first = std::move(*it);
				g_locations.erase(it);
				ensure(result.first->unmap());
				result.second = true;
				return result;
			}
		}

		return {};
	}

	std::shared_ptr<block_t> get(memory_location_t location, u32 addr)
	{
		vm::writer_lock lock;

		return _get_map(location, addr);
	}

	std::shared_ptr<block_t> reserve_map(memory_location_t location, u32 addr, u32 area_size, u64 flags)
	{
		vm::writer_lock lock;

		auto area = _get_map(location, addr);

		if (area)
		{
			return area;
		}

		// Allocation on arbitrary address
		if (location != any && location < g_locations.size())
		{
			// return selected location
			auto& loc = g_locations[location];

			if (!loc)
			{
				// Deferred allocation
				loc = _find_map(area_size, 0x10000000, flags);
			}

			return loc;
		}

		// Fixed address allocation
		area = _get_map(location, addr);

		if (area)
		{
			return area;
		}

		return _map(addr, area_size, flags);
	}

	bool try_access(u32 addr, void* ptr, u32 size, bool is_write)
	{
		vm::writer_lock lock;

		if (vm::check_addr(addr, is_write ? page_writable : page_readable, size))
		{
			void* src = vm::g_sudo_addr + addr;
			void* dst = ptr;

			if (is_write)
				std::swap(src, dst);

			if (size <= 16 && (size & (size - 1)) == 0 && (addr & (size - 1)) == 0)
			{
				if (is_write)
				{
					switch (size)
					{
					case 1: atomic_storage<u8>::release(*static_cast<u8*>(dst), *static_cast<u8*>(src)); break;
					case 2: atomic_storage<u16>::release(*static_cast<u16*>(dst), *static_cast<u16*>(src)); break;
					case 4: atomic_storage<u32>::release(*static_cast<u32*>(dst), *static_cast<u32*>(src)); break;
					case 8: atomic_storage<u64>::release(*static_cast<u64*>(dst), *static_cast<u64*>(src)); break;
					case 16: atomic_storage<u128>::release(*static_cast<u128*>(dst), *static_cast<u128*>(src)); break;
					}

					return true;
				}
			}

			std::memcpy(dst, src, size);
			return true;
		}

		return false;
	}

	inline namespace ps3_
	{
		static utils::shm s_hook{0x800000000, ""};

		void init()
		{
			vm_log.notice("Guest memory bases address ranges:\n"
			"vm::g_base_addr = %p - %p\n"
			"vm::g_sudo_addr = %p - %p\n"
			"vm::g_exec_addr = %p - %p\n"
			"vm::g_hook_addr = %p - %p\n"
			"vm::g_stat_addr = %p - %p\n"
			"vm::g_reservations = %p - %p\n",
			g_base_addr, g_base_addr + 0xffff'ffff,
			g_sudo_addr, g_sudo_addr + 0xffff'ffff,
			g_exec_addr, g_exec_addr + 0x200000000 - 1,
			g_hook_addr, g_hook_addr + 0x800000000 - 1,
			g_stat_addr, g_stat_addr + 0xffff'ffff,
			g_reservations, g_reservations + sizeof(g_reservations) - 1);

			std::memset(&g_pages, 0, sizeof(g_pages));

			g_locations =
			{
				std::make_shared<block_t>(0x00010000, 0x1FFF0000, page_size_64k | preallocated), // main
				std::make_shared<block_t>(0x20000000, 0x10000000, page_size_64k | bf0_0x1),		 // user 64k pages
				nullptr,                                                                         // user 1m pages
				nullptr,                                                                         // rsx context
				std::make_shared<block_t>(0xC0000000, 0x10000000, page_size_64k | preallocated), // video
				std::make_shared<block_t>(0xD0000000, 0x10000000, page_size_4k  | preallocated | stack_guarded | bf0_0x1), // stack
				std::make_shared<block_t>(0xE0000000, 0x20000000, page_size_64k),                // SPU reserved
			};

			std::memset(g_reservations, 0, sizeof(g_reservations));
			std::memset(g_shmem, 0, sizeof(g_shmem));
			std::memset(g_range_lock_set, 0, sizeof(g_range_lock_set));
			g_range_lock_bits = 0;

#ifdef _WIN32
			utils::memory_release(g_hook_addr, 0x800000000);
#endif
			ensure(s_hook.map(g_hook_addr, utils::protection::rw, true));
		}
	}

	void close()
	{
		{
			vm::writer_lock lock;

			for (auto& block : g_locations)
			{
				if (block) block->unmap();
			}

			g_locations.clear();
		}

		utils::memory_decommit(g_base_addr, 0x200000000);
		utils::memory_decommit(g_exec_addr, 0x200000000);
		utils::memory_decommit(g_stat_addr, 0x100000000);

#ifdef _WIN32
		s_hook.unmap(g_hook_addr);
		ensure(utils::memory_reserve(0x800000000, g_hook_addr));
#else
		utils::memory_decommit(g_hook_addr, 0x800000000);
#endif

		std::memset(g_range_lock_set, 0, sizeof(g_range_lock_set));
		g_range_lock_bits = 0;
	}
}

void fmt_class_string<vm::_ptr_base<const void, u32>>::format(std::string& out, u64 arg)
{
	fmt_class_string<u32>::format(out, arg);
}

void fmt_class_string<vm::_ptr_base<const char, u32>>::format(std::string& out, u64 arg)
{
	// Special case (may be allowed for some arguments)
	if (arg == 0)
	{
		out += reinterpret_cast<const char*>(u8"«NULL»");
		return;
	}

	// Filter certainly invalid addresses (TODO)
	if (arg < 0x10000 || arg >= 0xf0000000)
	{
		out += reinterpret_cast<const char*>(u8"«INVALID_ADDRESS:");
		fmt_class_string<u32>::format(out, arg);
		out += reinterpret_cast<const char*>(u8"»");
		return;
	}

	const auto start = out.size();

	out += reinterpret_cast<const char*>(u8"“");

	for (vm::_ptr_base<const volatile char, u32> ptr = vm::cast(arg);; ptr++)
	{
		if (!vm::check_addr(ptr.addr()))
		{
			// TODO: optimize checks
			out.resize(start);
			out += reinterpret_cast<const char*>(u8"«INVALID_ADDRESS:");
			fmt_class_string<u32>::format(out, arg);
			out += reinterpret_cast<const char*>(u8"»");
			return;
		}

		if (const char ch = *ptr)
		{
			out += ch;
		}
		else
		{
			break;
		}
	}

	out += reinterpret_cast<const char*>(u8"”");
}
