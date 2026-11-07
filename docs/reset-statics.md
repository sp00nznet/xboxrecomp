# Reset statics

Every piece of mutable C state in the toolkit, classified for the in-window
game reset (the `game-reset` design, D5). A reset stops every guest
thread, pauses the device threads at their loop heads, runs the reset hooks
by band, clears guest memory in place, reloads the XBE sections, re-runs the
guest-memory parts of the host-half inits and boots the title's entry point
again in the same process.

`tools/reset_statics.py` finds the state by reading the sources, and
`reset_statics.py check` fails on any object this file does not classify
(the `reset_statics` ctest runs it over the tree). A new static, thread
local or init-once guard therefore needs a row here in the same change.

The scanner does not preprocess. It blanks every preprocessor line and
scans both arms of each `#if`, so it lists an object under any
configuration and may list one twice. It does not see a static made by
a macro expansion. It warns on a function body it cannot name instead
of skipping it.

## Classes

- **keep**: host state that survives a reset unchanged. The window and its
  mode, the GPU device, queues, swap chain and present chain; shader and
  pipeline caches keyed by shader content; the host audio device and
  streams; cached config and env reads; menu, overlay, bindings and mod
  settings; log-once guards; profiler and stats totals; crash handlers;
  host locks that guard module state and are never held at a stop point;
  handles of device threads, which pause rather than exit.
- **reset**: anything derived from the guest or the running boot. Guest
  addresses and handles, kernel objects, timers, DPC and APC queues, IRQ
  state, clocks, PGRAPH shadows, the walker's carry-over, textures and
  surfaces cached from guest memory, APU, DSP, DSound and WMA state, OHCI and
  pad state, the video pump, the guest CPU lock, the deterministic mode, and
  the per-boot one-shots that gate guest-visible behaviour or run an init
  against guest memory. Debug-only per-run triggers are keep unless they
  hold guest pointers.
- **constant**: filled once and the same on every boot: lookup tables built
  from fixed data, parsed built-in data, function tables chosen from host
  capabilities.

The `hook` cell names the module hook that resets a `reset` row:
renderer.metal, renderer.d3d11, renderer.common, pgraph, hle_d3d8, apu,
dsp, dsound, input, usb, video, kernel, memory, xbe, guest_mem_init, clocks,
saves, mods, enhance or platform. For a pointer into guest memory, it is the
module that owns the pointer. For kind `once`, `why` starts with `rerun:`
(its init must run again) or `no rerun:` (a host-side init that is kept).

The key of a row is `file | scope | name`; line numbers are not part of it.

## Kernel (src/kernel, outside nv2a_*)

### src/kernel/guest_state_hash.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/guest_state_hash.c | - | s_log | file_static | keep | - | debug state-hash tool config and log; continues across reset |
| src/kernel/guest_state_hash.c | - | s_pages | file_static | keep | - | debug state-hash tool config and log; continues across reset |
| src/kernel/guest_state_hash.c | - | s_path | file_static | keep | - | debug state-hash tool config and log; continues across reset |
| src/kernel/guest_state_hash.c | - | s_skip_lo | file_static | keep | - | debug state-hash tool config and log; continues across reset |
| src/kernel/guest_state_hash.c | - | s_skip_hi | file_static | keep | - | debug state-hash tool config and log; continues across reset |
| src/kernel/guest_state_hash.c | - | s_nskips | file_static | keep | - | debug state-hash tool config and log; continues across reset |
| src/kernel/guest_state_hash.c | - | s_dump | file_static | keep | - | debug state-hash tool config and log; continues across reset |
| src/kernel/guest_state_hash.c | - | s_ndumps | file_static | keep | - | debug state-hash tool config and log; continues across reset |
| src/kernel/guest_state_hash.c | - | s_page_hash | file_static | keep | - | debug state-hash tool config and log; continues across reset |
| src/kernel/guest_state_hash.c | - | s_record | file_static | keep | - | debug state-hash tool config and log; continues across reset |
| src/kernel/guest_state_hash.c | - | s_npages | file_static | keep | - | debug state-hash tool config and log; continues across reset |
| src/kernel/guest_state_hash.c | - | s_first | file_static | keep | - | debug state-hash tool config and log; continues across reset |
| src/kernel/guest_state_hash.c | page_hash_at | buf | local_static | keep | - | scratch buffer for page hashing |

### src/kernel/guest_time.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/guest_time.c | - | s_mode | file_static | keep | - | virtual-time mode from env |
| src/kernel/guest_time.c | - | s_vblanks | file_static | reset | clocks | virtual vblank count, the guest clock base |
| src/kernel/guest_time.c | - | t_vt_last_v | tls | reset | clocks | boot thread virtual-time read pacing against s_vblanks |
| src/kernel/guest_time.c | - | t_vt_reads | tls | reset | clocks | boot thread virtual-time read pacing against s_vblanks |
| src/kernel/guest_time.c | vt_ns | warned | tls | keep | - | warn-once guard |
| src/kernel/guest_time.c | - | s_time_anchor_100ns | file_static | keep | - | wall-clock anchor for KeQuerySystemTime, absolute time |
| src/kernel/guest_time.c | - | s_time_anchor_counts | file_static | keep | - | wall-clock anchor for KeQuerySystemTime, absolute time |
| src/kernel/guest_time.c | - | s_time_freq_counts | file_static | keep | - | wall-clock anchor for KeQuerySystemTime, absolute time |
| src/kernel/guest_time.c | - | s_time_once | once | keep | - | no rerun: wall-clock anchor stays valid |
| src/kernel/guest_time.c | xbox_ReadTimeStampCounter | freq | local_static | constant | - | host performance-counter frequency |
| src/kernel/guest_time.c | xbox_ReadTimeStampCounter | origin | local_static | reset | clocks | TSC origin; guest TSC starts near zero each boot |

### src/kernel/guest_vmem.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/guest_vmem.c | - | s_pages | file_static | reset | memory | per-page guest reservation table; clear and decommit |
| src/kernel/guest_vmem.c | - | s_mirror_lo | file_static | keep | - | host reservation geometry of the ext VMA range |
| src/kernel/guest_vmem.c | - | s_lo | file_static | keep | - | host reservation geometry of the ext VMA range |
| src/kernel/guest_vmem.c | - | s_top | file_static | keep | - | host reservation geometry of the ext VMA range |
| src/kernel/guest_vmem.c | - | s_offset | file_static | keep | - | host reservation geometry of the ext VMA range |
| src/kernel/guest_vmem.c | - | s_lock | file_static | keep | - | host lock for the page table |

### src/kernel/icall_feedback.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/icall_feedback.c | - | g_icall_seen | global | keep | - | indirect-call diagnostic totals keyed by code VA |
| src/kernel/icall_feedback.c | - | g_icall_guard_hits | global | keep | - | indirect-call diagnostic totals keyed by code VA |
| src/kernel/icall_feedback.c | - | g_icall_guard_misses | global | keep | - | indirect-call diagnostic totals keyed by code VA |
| src/kernel/icall_feedback.c | - | g_icall_site_hs | global | keep | - | indirect-call diagnostic totals keyed by code VA |
| src/kernel/icall_feedback.c | - | g_icall_site_hl | global | keep | - | indirect-call diagnostic totals keyed by code VA |
| src/kernel/icall_feedback.c | - | s_sites | file_static | keep | - | indirect-call diagnostic totals keyed by code VA |
| src/kernel/icall_feedback.c | - | s_site_records | file_static | keep | - | indirect-call diagnostic totals keyed by code VA |
| src/kernel/icall_feedback.c | - | s_site_saturated | file_static | keep | - | indirect-call diagnostic totals keyed by code VA |

### src/kernel/kernel_bridge.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_bridge.c | bridge_buf_ok | seen | local_static | keep | - | warn-once per export |
| src/kernel/kernel_bridge.c | bridge_buf_ok | distinct | local_static | keep | - | warn-once per export |
| src/kernel/kernel_bridge.c | - | g_slot_ordinals | file_static | constant | - | ordinals read from the XBE thunk table, same each boot |
| src/kernel/kernel_bridge.c | - | g_ordinal_calls | file_static | keep | - | kernel call statistics |
| src/kernel/kernel_bridge.c | - | g_kernel_call_count | file_static | keep | - | kernel call statistics |
| src/kernel/kernel_bridge.c | kernel_log_budget | budget | local_static | keep | - | log budget |
| src/kernel/kernel_bridge.c | - | g_xbox_kernel_caller | tls | reset | kernel | guest return address of the current kernel call |
| src/kernel/kernel_bridge.c | - | g_thread_call_count | file_static | reset | kernel | PsCreateSystemThreadEx count; zero picks first-call behaviour |
| src/kernel/kernel_bridge.c | - | g_is_spawned_thread | tls | reset | kernel | thread is spawned; its guest stack top |
| src/kernel/kernel_bridge.c | - | g_thread_stack_top | tls | reset | kernel | thread is spawned; its guest stack top |
| src/kernel/kernel_bridge.c | - | g_defer_io_apc | file_static | keep | - | deferred I/O APC config |
| src/kernel/kernel_bridge.c | thread_set_os_name | set_desc | local_static | constant | - | host SetThreadDescription pointer lookup |
| src/kernel/kernel_bridge.c | thread_set_os_name | resolved | local_static | constant | - | host SetThreadDescription pointer lookup |
| src/kernel/kernel_bridge.c | xbox_log_thread_role | main_tid | local_static | keep | - | host main thread id for role logs |
| src/kernel/kernel_bridge.c | xbox_log_ms | t0 | local_static | keep | - | log timestamp origin |
| src/kernel/kernel_bridge.c | - | s_thobj_va | file_static | reset | kernel | guest thread objects carved from contiguous memory |
| src/kernel/kernel_bridge.c | - | s_thobj_refs | file_static | reset | kernel | guest thread objects carved from contiguous memory |
| src/kernel/kernel_bridge.c | - | s_thobj_count | file_static | reset | kernel | guest thread objects carved from contiguous memory |
| src/kernel/kernel_bridge.c | - | s_thobj_end_ev | file_static | reset | kernel | guest thread objects carved from contiguous memory |
| src/kernel/kernel_bridge.c | - | s_thobj_start_ev | file_static | reset | kernel | guest thread objects carved from contiguous memory |
| src/kernel/kernel_bridge.c | - | s_thobj_hold | file_static | reset | kernel | guest thread objects carved from contiguous memory |
| src/kernel/kernel_bridge.c | - | s_thobj_gated | file_static | reset | kernel | guest thread objects carved from contiguous memory |
| src/kernel/kernel_bridge.c | - | s_token_thobj | file_static | reset | kernel | guest thread objects carved from contiguous memory |
| src/kernel/kernel_bridge.c | - | s_thobj_lock | file_static | keep | - | host lock for thread objects |
| src/kernel/kernel_bridge.c | - | g_thread_obj_va | tls | reset | kernel | this thread's guest thread object VA |
| src/kernel/kernel_bridge.c | - | s_next_thread_id | file_static | reset | kernel | next unique guest thread id |
| src/kernel/kernel_bridge.c | - | g_thread_mode | file_static | keep | - | thread mode the game sets before boot |
| src/kernel/kernel_bridge.c | bridge_MmAllocateContiguousMemoryEx | n | local_static | keep | - | refusal log budget |
| src/kernel/kernel_bridge.c | bridge_NtAllocateVirtualMemory | warned | local_static | keep | - | warn-once guard |
| src/kernel/kernel_bridge.c | bridge_free_guest_pages | decommits | local_static | keep | - | free statistics |
| src/kernel/kernel_bridge.c | bridge_free_guest_pages | releases | local_static | keep | - | free statistics |
| src/kernel/kernel_bridge.c | bridge_free_guest_pages | refused | local_static | keep | - | free statistics |
| src/kernel/kernel_bridge.c | bridge_free_guest_pages | decommit_bytes | local_static | keep | - | free statistics |
| src/kernel/kernel_bridge.c | - | g_fscache_pages | file_static | reset | kernel | file cache size the guest set |
| src/kernel/kernel_bridge.c | bridge_NtWaitForSingleObjectEx | logged | local_static | keep | - | log budget |
| src/kernel/kernel_bridge.c | bridge_NtWaitForMultipleObjectsEx | logged | local_static | keep | - | log budget |
| src/kernel/kernel_bridge.c | - | g_dpc_queue | file_static | reset | kernel | pending guest DPC queue |
| src/kernel/kernel_bridge.c | - | g_dpc_head | file_static | reset | kernel | pending guest DPC queue |
| src/kernel/kernel_bridge.c | - | g_dpc_tail | file_static | reset | kernel | pending guest DPC queue |
| src/kernel/kernel_bridge.c | - | g_dpc_lock | file_static | keep | - | host lock for the DPC queue |
| src/kernel/kernel_bridge.c | - | g_dpc_lock_once | once | keep | - | no rerun: host lock init only |
| src/kernel/kernel_bridge.c | kernel_clock_ms | freq | local_static | constant | - | host counter frequency |
| src/kernel/kernel_bridge.c | - | g_vbl_ack_event | file_static | keep | - | host event of the vblank ack thread, which keeps running |
| src/kernel/kernel_bridge.c | - | g_vbl_ack_giveups | file_static | keep | - | vblank ack statistics |
| src/kernel/kernel_bridge.c | - | g_vbl_ack_late | file_static | keep | - | vblank ack statistics |
| src/kernel/kernel_bridge.c | - | g_vbl_ack_late_max | file_static | keep | - | vblank ack statistics |
| src/kernel/kernel_bridge.c | - | g_vbl_declined | file_static | keep | - | vblank ack statistics |
| src/kernel/kernel_bridge.c | - | g_vbl_tick_busy | file_static | reset | kernel | vblank ISR in-flight flag, last guest ack and ownership |
| src/kernel/kernel_bridge.c | - | g_vbl_ack_seen_ms | file_static | reset | kernel | vblank ISR in-flight flag, last guest ack and ownership |
| src/kernel/kernel_bridge.c | - | g_vbl_ack_owned | file_static | reset | kernel | vblank ISR in-flight flag, last guest ack and ownership |
| src/kernel/kernel_bridge.c | - | g_vbl_ack_once | once | keep | - | no rerun: ack thread and event stay |
| src/kernel/kernel_bridge.c | kernel_vblank_mode | mode | local_static | keep | - | vblank mode env cache |
| src/kernel/kernel_bridge.c | - | g_vblank_total | file_static | reset | clocks | vblanks delivered to the guest this boot |
| src/kernel/kernel_bridge.c | - | g_vblank_behind_ns | file_static | keep | - | vblank lateness statistics |
| src/kernel/kernel_bridge.c | - | g_vblank_behind_n | file_static | keep | - | vblank lateness statistics |
| src/kernel/kernel_bridge.c | - | g_vblank_late_max_ns | file_static | keep | - | vblank lateness statistics |
| src/kernel/kernel_bridge.c | kernel_vblank_clock_ms | v | local_static | keep | - | vblank clock mode env cache |
| src/kernel/kernel_bridge.c | kernel_vblank_count | total | local_static | keep | - | vblank rate report statistics |
| src/kernel/kernel_bridge.c | kernel_vblank_count | last_ms | local_static | keep | - | vblank rate report statistics |
| src/kernel/kernel_bridge.c | kernel_vblank_count | qf | local_static | keep | - | vblank rate report statistics |
| src/kernel/kernel_bridge.c | kernel_vblank_count | qlast | local_static | keep | - | vblank rate report statistics |
| src/kernel/kernel_bridge.c | kernel_vblank_count | gap_min | local_static | keep | - | vblank rate report statistics |
| src/kernel/kernel_bridge.c | kernel_vblank_count | gap_max | local_static | keep | - | vblank rate report statistics |
| src/kernel/kernel_bridge.c | kernel_vblank_deliver | n | local_static | keep | - | log budget |
| src/kernel/kernel_bridge.c | kernel_vblank_raise | said | local_static | keep | - | log-once guard |
| src/kernel/kernel_bridge.c | - | g_gr_delivered | file_static | reset | pgraph | PGRAPH notify interrupts delivered and claimed |
| src/kernel/kernel_bridge.c | - | g_gr_claimed | file_static | reset | pgraph | PGRAPH notify interrupts delivered and claimed |
| src/kernel/kernel_bridge.c | kernel_vblank_tick | t0 | local_static | reset | clocks | vblank tick origin and count since boot |
| src/kernel/kernel_bridge.c | kernel_vblank_tick | count | local_static | reset | clocks | vblank tick origin and count since boot |
| src/kernel/kernel_bridge.c | kernel_vblank_tick_ns | sched | local_static | reset | clocks | next scheduled vblank time |
| src/kernel/kernel_bridge.c | kernel_vblank_tick_legacy | next_ms | local_static | reset | clocks | legacy vblank tick schedule |
| src/kernel/kernel_bridge.c | kernel_vblank_tick_legacy | last_ms | local_static | reset | clocks | legacy vblank tick schedule |
| src/kernel/kernel_bridge.c | kernel_vblank_tick_legacy | n | local_static | keep | - | legacy tick counter for logs |
| src/kernel/kernel_bridge.c | - | g_connected_isr | file_static | reset | kernel | guest KINTERRUPT VAs per vector |
| src/kernel/kernel_bridge.c | - | g_timers | file_static | reset | kernel | guest KTIMER and DPC VAs with due times |
| src/kernel/kernel_bridge.c | - | g_timer_lock | file_static | keep | - | host lock for the timer table |
| src/kernel/kernel_bridge.c | - | g_timer_started | once | keep | - | no rerun: timer thread keeps running |
| src/kernel/kernel_bridge.c | kernel_timer_paced | paced | local_static | keep | - | env cache |
| src/kernel/kernel_bridge.c | bridge_get_xbox_path | path | tls | keep | - | per-thread path scratch buffer |
| src/kernel/kernel_bridge.c | - | s_handle_table | file_static | reset | kernel | guest handle tokens to host HANDLEs; close them |
| src/kernel/kernel_bridge.c | - | s_handle_lock | file_static | keep | - | host spin lock for the handle table |
| src/kernel/kernel_bridge.c | - | xbox_FileOpenHook | global | keep | - | host file-open hook a game or mod sets |
| src/kernel/kernel_bridge.c | - | g_async_handles | file_static | reset | kernel | guest file handles opened for async I/O |
| src/kernel/kernel_bridge.c | bridge_async_io_enabled | on | local_static | keep | - | env cache |
| src/kernel/kernel_bridge.c | bridge_file_trace | on | local_static | keep | - | env cache |
| src/kernel/kernel_bridge.c | bridge_NtSetInformationFile | path | tls | keep | - | per-thread path scratch buffer |
| src/kernel/kernel_bridge.c | bridge_NtQuerySymbolicLinkObject | warned | local_static | keep | - | warn-once guard |
| src/kernel/kernel_bridge.c | bridge_RtlRaiseException | raise_count | local_static | keep | - | exception log counter |
| src/kernel/kernel_bridge.c | bridge_sched_resume_unknown | said | local_static | keep | - | log-once guard |
| src/kernel/kernel_bridge.c | bridge_NtDuplicateObject | logged | local_static | keep | - | log budget |
| src/kernel/kernel_bridge.c | bridge_XboxEEPROMKey | eeprom_va | local_static | reset | kernel | EEPROM key VA in the guest heap, which is cleared |
| src/kernel/kernel_bridge.c | - | g_ke_shadow | file_static | reset | kernel | guest Ke object VAs to host handles; close them |
| src/kernel/kernel_bridge.c | - | g_ke_shadow_cs | file_static | keep | - | host lock for the Ke shadow table |
| src/kernel/kernel_bridge.c | - | g_ke_shadow_init | once | keep | - | no rerun: host lock init only |
| src/kernel/kernel_bridge.c | ke_guest_event | enabled | local_static | keep | - | Ke event trace env cache and log guard |
| src/kernel/kernel_bridge.c | ke_guest_event | shown | local_static | keep | - | Ke event trace env cache and log guard |
| src/kernel/kernel_bridge.c | - | g_slot_bridges | file_static | constant | - | per-slot bridge functions and arg sizes from XBE ordinals |
| src/kernel/kernel_bridge.c | - | g_slot_arg_bytes | file_static | constant | - | per-slot bridge functions and arg sizes from XBE ordinals |
| src/kernel/kernel_bridge.c | - | g_slot_arg_unknown | file_static | constant | - | per-slot bridge functions and arg sizes from XBE ordinals |
| src/kernel/kernel_bridge.c | - | g_kernel_watch_va | global | keep | - | RECOMP_KERNEL_WATCH debug address |
| src/kernel/kernel_bridge.c | kernel_watch_arm_once | done | local_static | keep | - | env read-once guard |
| src/kernel/kernel_bridge.c | - | g_kernel_dispatch_slot | tls | reset | kernel | slot being dispatched on this thread |
| src/kernel/kernel_bridge.c | kernel_summary_trace | on | local_static | keep | - | env cache |
| src/kernel/kernel_bridge.c | - | s_last_ordinal | file_static | keep | - | last-call summary for crash diagnostics |
| src/kernel/kernel_bridge.c | - | s_last_slot | file_static | keep | - | last-call summary for crash diagnostics |
| src/kernel/kernel_bridge.c | - | s_last_esp | file_static | keep | - | last-call summary for crash diagnostics |
| src/kernel/kernel_bridge.c | - | s_summary_hooked | file_static | keep | - | last-call summary for crash diagnostics |
| src/kernel/kernel_bridge.c | - | s_summary_done | file_static | keep | - | last-call summary for crash diagnostics |
| src/kernel/kernel_bridge.c | kernel_thunk_dispatch_body | last_summary_tick | local_static | keep | - | dispatch log guards and summary timer |
| src/kernel/kernel_bridge.c | kernel_thunk_dispatch_body | seen | local_static | keep | - | dispatch log guards and summary timer |
| src/kernel/kernel_bridge.c | kernel_thunk_dispatch_body | warned | local_static | keep | - | dispatch log guards and summary timer |
| src/kernel/kernel_bridge.c | - | g_thunk_table_base | file_static | constant | - | thunk table address parsed from the XBE header |
| src/kernel/kernel_bridge.c | - | g_thunk_table_count | file_static | constant | - | thunk table address parsed from the XBE header |
| src/kernel/kernel_bridge.c | - | g_ordinal_remap | file_static | constant | - | per-title ordinal remap the game sets |
| src/kernel/kernel_bridge.c | - | g_ordinal_remap_count | file_static | constant | - | per-title ordinal remap the game sets |

### src/kernel/kernel_file.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_file.c | - | g_xbox_last_file_error | global | reset | kernel | last guest file error status |
| src/kernel/kernel_file.c | - | s_overwrite_pending | file_static | reset | kernel | pending overwrite host file handles from guest opens |
| src/kernel/kernel_file.c | - | s_overwrite_lock | file_static | keep | - | host lock guarding module state |
| src/kernel/kernel_file.c | - | s_dir_contexts | file_static | reset | kernel | open guest directory enumeration contexts |
| src/kernel/kernel_file.c | - | s_dir_cs | file_static | keep | - | host lock guarding module state |
| src/kernel/kernel_file.c | - | s_dir_cs_init | once | keep | - | no rerun: host lock init only |
| src/kernel/kernel_file.c | host_NtCreateFile | warned | local_static | keep | - | warn-once guard |
| src/kernel/kernel_file.c | - | s_saves_h | file_static | reset | saves | open save-file handles; thaw and close at reset |
| src/kernel/kernel_file.c | - | s_saves_n | file_static | reset | saves | open save-file handles; thaw and close at reset |
| src/kernel/kernel_file.c | - | s_saves_stamp | file_static | keep | - | save write stamp and generation, monotonic host counters |
| src/kernel/kernel_file.c | - | s_saves_gen | file_static | keep | - | save write stamp and generation, monotonic host counters |
| src/kernel/kernel_file.c | - | s_saves_lock | file_static | keep | - | host lock guarding module state |

### src/kernel/kernel_guest_cpu.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_guest_cpu.c | - | g_xbox_guest_cpu_waiters | global | reset | kernel | guest CPU lock waiter count and preempt request |
| src/kernel/kernel_guest_cpu.c | - | g_xbox_guest_cpu_preempt | global | reset | kernel | guest CPU lock waiter count and preempt request |
| src/kernel/kernel_guest_cpu.c | - | s_lock | file_static | keep | - | host lock and CV for the guest CPU lock |
| src/kernel/kernel_guest_cpu.c | - | s_cv | file_static | keep | - | host lock and CV for the guest CPU lock |
| src/kernel/kernel_guest_cpu.c | - | s_once | once | keep | - | no rerun: env config read only |
| src/kernel/kernel_guest_cpu.c | - | s_on | file_static | keep | - | guest CPU lock config |
| src/kernel/kernel_guest_cpu.c | - | s_trace_ns | file_static | keep | - | guest CPU lock config |
| src/kernel/kernel_guest_cpu.c | - | s_quantum_ns | file_static | keep | - | guest CPU lock config |
| src/kernel/kernel_guest_cpu.c | - | s_account | file_static | keep | - | guest CPU lock config |
| src/kernel/kernel_guest_cpu.c | - | s_sched | file_static | keep | - | guest CPU lock config |
| src/kernel/kernel_guest_cpu.c | - | s_free | file_static | reset | kernel | ticket FIFO and owner of the guest CPU lock |
| src/kernel/kernel_guest_cpu.c | - | s_granted | file_static | reset | kernel | ticket FIFO and owner of the guest CPU lock |
| src/kernel/kernel_guest_cpu.c | - | s_next_ticket | file_static | reset | kernel | ticket FIFO and owner of the guest CPU lock |
| src/kernel/kernel_guest_cpu.c | - | s_head | file_static | reset | kernel | ticket FIFO and owner of the guest CPU lock |
| src/kernel/kernel_guest_cpu.c | - | s_waiting | file_static | reset | kernel | ticket FIFO and owner of the guest CPU lock |
| src/kernel/kernel_guest_cpu.c | - | s_owner_tid | file_static | reset | kernel | ticket FIFO and owner of the guest CPU lock |
| src/kernel/kernel_guest_cpu.c | - | t_guest | tls | reset | kernel | per-thread guest CPU lock hold and turn state |
| src/kernel/kernel_guest_cpu.c | - | t_held | tls | reset | kernel | per-thread guest CPU lock hold and turn state |
| src/kernel/kernel_guest_cpu.c | - | t_turn_open | tls | reset | kernel | per-thread guest CPU lock hold and turn state |
| src/kernel/kernel_guest_cpu.c | - | t_turn_ns | tls | reset | kernel | per-thread guest CPU lock hold and turn state |
| src/kernel/kernel_guest_cpu.c | - | t_soft_ns | tls | reset | kernel | per-thread guest CPU lock hold and turn state |
| src/kernel/kernel_guest_cpu.c | - | t_soft_kind | tls | reset | kernel | per-thread guest CPU lock hold and turn state |
| src/kernel/kernel_guest_cpu.c | - | t_soft_where | tls | reset | kernel | per-thread guest CPU lock hold and turn state |
| src/kernel/kernel_guest_cpu.c | - | t_turn_cpu0 | tls | reset | kernel | per-thread guest CPU lock hold and turn state |
| src/kernel/kernel_guest_cpu.c | - | t_run_ns | tls | reset | kernel | per-thread guest CPU lock hold and turn state |
| src/kernel/kernel_guest_cpu.c | - | s_threads | file_static | keep | - | per-tid statistics records |
| src/kernel/kernel_guest_cpu.c | - | t_rec | tls | keep | - | per-tid statistics records |
| src/kernel/kernel_guest_cpu.c | - | s_turns | file_static | keep | - | guest CPU lock statistics |
| src/kernel/kernel_guest_cpu.c | - | s_releases | file_static | keep | - | guest CPU lock statistics |
| src/kernel/kernel_guest_cpu.c | - | s_switches | file_static | keep | - | guest CPU lock statistics |
| src/kernel/kernel_guest_cpu.c | - | s_waits | file_static | keep | - | guest CPU lock statistics |
| src/kernel/kernel_guest_cpu.c | - | s_yields | file_static | keep | - | guest CPU lock statistics |
| src/kernel/kernel_guest_cpu.c | - | s_preempts | file_static | keep | - | guest CPU lock statistics |
| src/kernel/kernel_guest_cpu.c | - | s_max_turn_us | file_static | keep | - | guest CPU lock statistics |
| src/kernel/kernel_guest_cpu.c | - | s_max_wait_us | file_static | keep | - | guest CPU lock statistics |
| src/kernel/kernel_guest_cpu.c | - | s_max_wait_run_us | file_static | keep | - | guest CPU lock statistics |
| src/kernel/kernel_guest_cpu.c | - | s_max_turn_kind | file_static | keep | - | guest CPU lock statistics |
| src/kernel/kernel_guest_cpu.c | - | s_max_turn_where | file_static | keep | - | guest CPU lock statistics |
| src/kernel/kernel_guest_cpu.c | - | s_trace_lines | file_static | keep | - | guest CPU lock statistics |
| src/kernel/kernel_guest_cpu.c | - | s_trace_last_ns | file_static | keep | - | guest CPU lock statistics |
| src/kernel/kernel_guest_cpu.c | - | s_sched_threads | file_static | reset | kernel | guest_sched wait queue, holder and soft hold |
| src/kernel/kernel_guest_cpu.c | - | t_sched | tls | reset | kernel | guest_sched wait queue, holder and soft hold |
| src/kernel/kernel_guest_cpu.c | - | s_wait_head | file_static | reset | kernel | guest_sched wait queue, holder and soft hold |
| src/kernel/kernel_guest_cpu.c | - | s_wait_tail | file_static | reset | kernel | guest_sched wait queue, holder and soft hold |
| src/kernel/kernel_guest_cpu.c | - | s_sched_ticket | file_static | reset | kernel | guest_sched wait queue, holder and soft hold |
| src/kernel/kernel_guest_cpu.c | - | s_holder | file_static | reset | kernel | guest_sched wait queue, holder and soft hold |
| src/kernel/kernel_guest_cpu.c | - | s_soft_tid | file_static | reset | kernel | guest_sched wait queue, holder and soft hold |
| src/kernel/kernel_guest_cpu.c | - | s_soft_level | file_static | reset | kernel | guest_sched wait queue, holder and soft hold |
| src/kernel/kernel_guest_cpu.c | - | s_soft_blocking | file_static | reset | kernel | guest_sched wait queue, holder and soft hold |
| src/kernel/kernel_guest_cpu.c | - | s_soft_kind | file_static | reset | kernel | guest_sched wait queue, holder and soft hold |
| src/kernel/kernel_guest_cpu.c | - | s_last_tid | file_static | keep | - | last switch for trace output |
| src/kernel/kernel_guest_cpu.c | - | s_last_level | file_static | keep | - | last switch for trace output |
| src/kernel/kernel_guest_cpu.c | - | s_soft_where | file_static | reset | kernel | guest_sched wait queue, holder and soft hold |
| src/kernel/kernel_guest_cpu.c | - | s_soft_at_ns | file_static | reset | kernel | guest_sched wait queue, holder and soft hold |
| src/kernel/kernel_guest_cpu.c | - | s_refused | file_static | keep | - | scheduler statistics and log budgets |
| src/kernel/kernel_guest_cpu.c | - | s_starve_lines | file_static | keep | - | scheduler statistics and log budgets |
| src/kernel/kernel_guest_cpu.c | - | s_starve_last_ns | file_static | keep | - | scheduler statistics and log budgets |
| src/kernel/kernel_guest_cpu.c | - | s_sched_takes | file_static | keep | - | scheduler statistics and log budgets |
| src/kernel/kernel_guest_cpu.c | - | s_sched_blocking | file_static | keep | - | scheduler statistics and log budgets |
| src/kernel/kernel_guest_cpu.c | - | s_site_kind | file_static | keep | - | scheduler statistics and log budgets |
| src/kernel/kernel_guest_cpu.c | - | s_site_where | file_static | keep | - | scheduler statistics and log budgets |
| src/kernel/kernel_guest_cpu.c | - | s_soft_ns | file_static | keep | - | scheduler statistics and log budgets |
| src/kernel/kernel_guest_cpu.c | - | s_valve_ns | file_static | keep | - | scheduler statistics and log budgets |
| src/kernel/kernel_guest_cpu.c | - | s_late | file_static | keep | - | scheduler statistics and log budgets |
| src/kernel/kernel_guest_cpu.c | - | s_late_lines | file_static | keep | - | scheduler statistics and log budgets |
| src/kernel/kernel_guest_cpu.c | - | s_late_last_ns | file_static | keep | - | scheduler statistics and log budgets |
| src/kernel/kernel_guest_cpu.c | - | s_valve | file_static | reset | kernel | valve points at a waiter on an exited thread stack |
| src/kernel/kernel_guest_cpu.c | - | s_valve_tid | file_static | reset | kernel | valve points at a waiter on an exited thread stack |
| src/kernel/kernel_guest_cpu.c | - | s_valve_until_ns | file_static | reset | kernel | valve points at a waiter on an exited thread stack |
| src/kernel/kernel_guest_cpu.c | - | s_valves | file_static | keep | - | valve statistics and log guards |
| src/kernel/kernel_guest_cpu.c | - | s_valves_total | file_static | keep | - | valve statistics and log guards |
| src/kernel/kernel_guest_cpu.c | - | s_table_full_said | file_static | keep | - | valve statistics and log guards |
| src/kernel/kernel_guest_cpu.c | - | t_note | tls | keep | - | valve statistics and log guards |
| src/kernel/kernel_guest_cpu.c | - | t_joining | tls | reset | kernel | thread is joining the guest CPU lock |

### src/kernel/kernel_guest_det.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_guest_det.c | - | s_once | once | keep | - | no rerun: det-mode env init only |
| src/kernel/kernel_guest_det.c | - | s_lock | file_static | keep | - | det-mode host lock, CV and config |
| src/kernel/kernel_guest_det.c | - | s_cv | file_static | keep | - | det-mode host lock, CV and config |
| src/kernel/kernel_guest_det.c | - | s_on | file_static | keep | - | det-mode host lock, CV and config |
| src/kernel/kernel_guest_det.c | - | s_armed | file_static | keep | - | det-mode host lock, CV and config |
| src/kernel/kernel_guest_det.c | - | s_slot | file_static | reset | kernel | det scheduler slots and turn of guest threads |
| src/kernel/kernel_guest_det.c | - | s_count | file_static | reset | kernel | det scheduler slots and turn of guest threads |
| src/kernel/kernel_guest_det.c | - | s_turn | file_static | reset | kernel | det scheduler slots and turn of guest threads |
| src/kernel/kernel_guest_det.c | - | s_idle | file_static | reset | kernel | det scheduler slots and turn of guest threads |
| src/kernel/kernel_guest_det.c | - | s_dev | file_static | reset | kernel | device slots; keep present flags, device threads keep running |
| src/kernel/kernel_guest_det.c | - | s_steps | file_static | reset | kernel | det step counts; per-thread edges compare against them |
| src/kernel/kernel_guest_det.c | - | s_catchups | file_static | reset | kernel | det step counts; per-thread edges compare against them |
| src/kernel/kernel_guest_det.c | - | s_passes | file_static | reset | kernel | det step counts; per-thread edges compare against them |
| src/kernel/kernel_guest_det.c | - | s_trace | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | s_trace_pages | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | s_trace_path | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | s_trace_dump | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | s_trace_ndumps | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | s_trace_hash | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | s_trace_npages | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | s_snap | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | s_snap_from | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | s_snap_to | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | s_snap_va | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | s_snap_n | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | t_slot | tls | reset | kernel | per-thread det slot binding and edges |
| src/kernel/kernel_guest_det.c | - | t_bind | tls | reset | kernel | per-thread det slot binding and edges |
| src/kernel/kernel_guest_det.c | - | t_edges | tls | reset | kernel | per-thread det slot binding and edges |
| src/kernel/kernel_guest_det.c | - | t_edge | tls | reset | kernel | per-thread det slot binding and edges |
| src/kernel/kernel_guest_det.c | - | s_prof | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | s_pf_last_ns | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | s_pf_dev_ns | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | s_pf_hash_ns | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | s_pf_flips | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | s_pf_held | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | s_pf_catch0 | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | s_pf_pass0 | file_static | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | t_pf_kind | tls | keep | - | det trace, snapshot and profile debug state |
| src/kernel/kernel_guest_det.c | - | t_clock_mark | tls | reset | kernel | per-thread det slot binding and edges |

### src/kernel/kernel_hal.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_hal.c | - | g_current_irql | tls | reset | kernel | per-thread IRQL and pending release; boot thread reused |
| src/kernel/kernel_hal.c | - | t_release_due | tls | reset | kernel | per-thread IRQL and pending release; boot thread reused |
| src/kernel/kernel_hal.c | - | g_irql_raised_count | file_static | reset | kernel | count of raised threads; stays raised if a thread unwinds raised |
| src/kernel/kernel_hal.c | - | s_trace | file_static | keep | - | IRQL trace config and counts |
| src/kernel/kernel_hal.c | - | s_traced | file_static | keep | - | IRQL trace config and counts |
| src/kernel/kernel_hal.c | - | s_transitions | file_static | keep | - | IRQL trace config and counts |
| src/kernel/kernel_hal.c | - | s_holders | file_static | reset | kernel | threads holding raised IRQL |
| src/kernel/kernel_hal.c | - | t_irql_counted | tls | reset | kernel | whether this thread counts in the raised total |
| src/kernel/kernel_hal.c | - | s_gate_on | file_static | keep | - | dispatch gate config |
| src/kernel/kernel_hal.c | - | s_dispatch_gate | file_static | keep | - | host CS reused; RISK: may be held by an unwinding thread |
| src/kernel/kernel_hal.c | - | s_dispatch_gate_once | once | keep | - | no rerun: host CS init only |
| src/kernel/kernel_hal.c | - | t_dispatch_gate_held | tls | reset | kernel | dispatch gate holder and depth |
| src/kernel/kernel_hal.c | - | t_gate_depth | tls | reset | kernel | dispatch gate holder and depth |
| src/kernel/kernel_hal.c | - | s_gate_owner | file_static | reset | kernel | dispatch gate holder and depth |
| src/kernel/kernel_hal.c | - | g_xbox_irq_pending | global | reset | kernel | pending device IRQ lines and post times |
| src/kernel/kernel_hal.c | - | s_irq_run | file_static | constant | - | host IRQ handler function pointers set at init |
| src/kernel/kernel_hal.c | - | s_irq_post_ns | file_static | reset | kernel | pending device IRQ lines and post times |
| src/kernel/kernel_hal.c | - | t_in_isr | tls | reset | kernel | thread is inside an ISR or DPC |
| src/kernel/kernel_hal.c | - | s_irq_post_hook | file_static | constant | - | host IRQ handler function pointers set at init |
| src/kernel/kernel_hal.c | - | s_irq_release_hook | file_static | constant | - | host IRQ handler function pointers set at init |
| src/kernel/kernel_hal.c | - | s_irq_mode | file_static | keep | - | IRQ config from env |
| src/kernel/kernel_hal.c | - | s_irq_force | file_static | keep | - | IRQ config from env |
| src/kernel/kernel_hal.c | - | s_dpc_on | file_static | keep | - | IRQ config from env |
| src/kernel/kernel_hal.c | - | t_guest_thread | tls | reset | kernel | marks this thread as a guest thread |
| src/kernel/kernel_hal.c | - | t_in_dpc | tls | reset | kernel | thread is inside an ISR or DPC |
| src/kernel/kernel_hal.c | - | s_irq_wait_ns | file_static | keep | - | IRQ config from env |
| src/kernel/kernel_hal.c | - | s_irq | file_static | keep | - | IRQ statistics |
| src/kernel/kernel_hal.c | - | s_irq_points_refused | file_static | keep | - | IRQ statistics |
| src/kernel/kernel_hal.c | - | s_irq_last_log_ns | file_static | keep | - | IRQ statistics |
| src/kernel/kernel_hal.c | - | s_irq_config_once | once | keep | - | no rerun: env config read only |
| src/kernel/kernel_hal.c | irq_run_line | said | local_static | keep | - | log-once guard |
| src/kernel/kernel_hal.c | xbox_IrqNoteUngated | said | local_static | keep | - | log-once guard |
| src/kernel/kernel_hal.c | xbox_KfLowerIrql | n | local_static | keep | - | log budget counter |
| src/kernel/kernel_hal.c | - | xbox_KeTickCount | global | reset | clocks | host-native tick count since boot |
| src/kernel/kernel_hal.c | - | g_av_saved_data_address | file_static | reset | kernel | guest VA set by AvSetSavedDataAddress |
| src/kernel/kernel_hal.c | - | g_av_display_mode | file_static | reset | kernel | display mode the guest set |
| src/kernel/kernel_hal.c | - | xbox_HalDiskCachePartitionCount | global | constant | - | fixed host-native HAL data exports |
| src/kernel/kernel_hal.c | - | g_disk_model | file_static | constant | - | fixed host-native HAL data exports |
| src/kernel/kernel_hal.c | - | g_disk_serial | file_static | constant | - | fixed host-native HAL data exports |
| src/kernel/kernel_hal.c | - | xbox_HalDiskModelNumber | global | constant | - | fixed host-native HAL data exports |
| src/kernel/kernel_hal.c | - | xbox_HalDiskSerialNumber | global | constant | - | fixed host-native HAL data exports |
| src/kernel/kernel_hal.c | - | xbox_HalBootSMCVideoMode | global | constant | - | fixed host-native HAL data exports |
| src/kernel/kernel_hal.c | - | g_idex_channel_data | file_static | constant | - | fixed host-native HAL data exports |
| src/kernel/kernel_hal.c | - | xbox_IdexChannelObject | global | constant | - | fixed host-native HAL data exports |
| src/kernel/kernel_hal.c | port_first_use | seen | local_static | keep | - | first-use port log guard |
| src/kernel/kernel_hal.c | port_first_use | nseen | local_static | keep | - | first-use port log guard |
| src/kernel/kernel_hal.c | xbox_PortIn | reads | local_static | keep | - | port read log counter |

### src/kernel/kernel_io.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_io.c | - | g_device_object_type_data | file_static | constant | - | fixed object type descriptors |
| src/kernel/kernel_io.c | - | g_completion_object_type_data | file_static | constant | - | fixed object type descriptors |
| src/kernel/kernel_io.c | - | g_file_object_type_data | file_static | constant | - | fixed object type descriptors |
| src/kernel/kernel_io.c | - | xbox_IoDeviceObjectType | global | constant | - | fixed object type descriptors |
| src/kernel/kernel_io.c | - | xbox_IoCompletionObjectType | global | constant | - | fixed object type descriptors |
| src/kernel/kernel_io.c | - | xbox_IoFileObjectType | global | constant | - | fixed object type descriptors |
| src/kernel/kernel_io.c | - | g_symlinks | file_static | reset | kernel | guest-created symbolic links |

### src/kernel/kernel_language.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_language.c | - | s_shipped | file_static | constant | - | shipped language table built once from fixed data |
| src/kernel/kernel_language.c | - | s_shipped_built | file_static | constant | - | shipped language table built once from fixed data |
| src/kernel/kernel_language.c | - | s_lang | file_static | keep | - | configured language |

### src/kernel/kernel_missing.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_missing.c | - | s_slots | file_static | keep | - | missing-function diagnostics, totals and said guards |
| src/kernel/kernel_missing.c | - | s_lock | file_static | keep | - | missing-function diagnostics, totals and said guards |
| src/kernel/kernel_missing.c | - | s_high | file_static | keep | - | missing-function diagnostics, totals and said guards |
| src/kernel/kernel_missing.c | - | s_low | file_static | keep | - | missing-function diagnostics, totals and said guards |
| src/kernel/kernel_missing.c | - | s_other | file_static | keep | - | missing-function diagnostics, totals and said guards |
| src/kernel/kernel_missing.c | - | s_probe | file_static | keep | - | missing-function diagnostics, totals and said guards |
| src/kernel/kernel_missing.c | - | s_attempts | file_static | keep | - | missing-function diagnostics, totals and said guards |
| src/kernel/kernel_missing.c | - | s_repeats | file_static | keep | - | missing-function diagnostics, totals and said guards |
| src/kernel/kernel_missing.c | - | s_unnamed | file_static | keep | - | missing-function diagnostics, totals and said guards |
| src/kernel/kernel_missing.c | - | s_cap_said | file_static | keep | - | missing-function diagnostics, totals and said guards |
| src/kernel/kernel_missing.c | - | s_summary_done | file_static | keep | - | missing-function diagnostics, totals and said guards |
| src/kernel/kernel_missing.c | - | s_list_done | file_static | keep | - | missing-function diagnostics, totals and said guards |
| src/kernel/kernel_missing.c | - | s_atexit_set | file_static | keep | - | missing-function diagnostics, totals and said guards |
| src/kernel/kernel_missing.c | - | s_mode | file_static | keep | - | env cache for missing-function mode |

### src/kernel/kernel_ob.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_ob.c | - | g_thread_object_type_data | file_static | constant | - | fixed object type descriptors |
| src/kernel/kernel_ob.c | - | g_event_object_type_data | file_static | constant | - | fixed object type descriptors |
| src/kernel/kernel_ob.c | - | xbox_PsThreadObjectType | global | constant | - | fixed object type descriptors |
| src/kernel/kernel_ob.c | - | xbox_ExEventObjectType | global | constant | - | fixed object type descriptors |

### src/kernel/kernel_pacing.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_pacing.c | - | g_xbox_spin_sleep | global | keep | - | spin-sleep mode from env |
| src/kernel/kernel_pacing.c | - | g_xbox_kick_armed | global | keep | - | kick thread walker-asleep flag; the thread keeps running |
| src/kernel/kernel_pacing.c | - | s_lock | file_static | keep | - | host lock and CV for pacing |
| src/kernel/kernel_pacing.c | - | s_cv | file_static | keep | - | host lock and CV for pacing |
| src/kernel/kernel_pacing.c | - | s_once | once | keep | - | no rerun: host pacing init only |
| src/kernel/kernel_pacing.c | - | s_gen | file_static | keep | - | pacing wake generation, monotonic |
| src/kernel/kernel_pacing.c | - | t_seen_gen | tls | keep | - | pacing wake generation, monotonic |
| src/kernel/kernel_pacing.c | - | s_sites | file_static | keep | - | pacing trace and stats |
| src/kernel/kernel_pacing.c | - | t_last_site | tls | keep | - | pacing trace and stats |
| src/kernel/kernel_pacing.c | - | t_last_outcome | tls | keep | - | pacing trace and stats |
| src/kernel/kernel_pacing.c | - | t_last_ret_ns | tls | keep | - | pacing trace and stats |
| src/kernel/kernel_pacing.c | - | s_trace | file_static | keep | - | pacing trace and stats |
| src/kernel/kernel_pacing.c | - | s_gap_min_us | file_static | keep | - | pacing trace and stats |
| src/kernel/kernel_pacing.c | - | s_gap_max_us | file_static | keep | - | pacing trace and stats |
| src/kernel/kernel_pacing.c | - | s_last_vblank_ns | file_static | keep | - | pacing trace and stats |
| src/kernel/kernel_pacing.c | xbox_PacingFlip | last_ns | local_static | keep | - | flip pacing stats window |
| src/kernel/kernel_pacing.c | xbox_PacingFlip | win_ns | local_static | keep | - | flip pacing stats window |
| src/kernel/kernel_pacing.c | xbox_PacingFlip | win_cpu | local_static | keep | - | flip pacing stats window |
| src/kernel/kernel_pacing.c | xbox_PacingFlip | iv_us | local_static | keep | - | flip pacing stats window |
| src/kernel/kernel_pacing.c | xbox_PacingFlip | n | local_static | keep | - | flip pacing stats window |

### src/kernel/kernel_path.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_path.c | - | g_xbox_path_hook | global | keep | - | host path config resolved at path init |
| src/kernel/kernel_path.c | - | s_user_in_game | file_static | keep | - | host path config resolved at path init |
| src/kernel/kernel_path.c | - | s_last_host_path_shared | tls | keep | - | per-thread host path scratch buffer |
| src/kernel/kernel_path.c | xbox_partition_device_digit | prefix | local_static | constant | - | fixed partition prefix table |
| src/kernel/kernel_path.c | - | s_title_id | file_static | keep | - | host path config resolved at path init |
| src/kernel/kernel_path.c | - | s_title_name | file_static | keep | - | host path config resolved at path init |
| src/kernel/kernel_path.c | - | s_game_dir | file_static | keep | - | host path config resolved at path init |
| src/kernel/kernel_path.c | - | s_save_dir | file_static | keep | - | host path config resolved at path init |
| src/kernel/kernel_path.c | - | s_user_dir | file_static | keep | - | host path config resolved at path init |
| src/kernel/kernel_path.c | - | s_initialized | file_static | keep | - | host path config resolved at path init |
| src/kernel/kernel_path.c | xbox_path_init | subs | local_static | constant | - | fixed path substitution tables |
| src/kernel/kernel_path.c | xbox_path_init | names | local_static | constant | - | fixed path substitution tables |
| src/kernel/kernel_path.c | - | s_case_warned | file_static | keep | - | host path config resolved at path init |
| src/kernel/kernel_path.c | xbox_path_adopt_legacy_saves | names | local_static | constant | - | fixed legacy save names |

### src/kernel/kernel_prof.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_prof.c | - | xbox_prof_state_ | global | keep | - | kernel profiler totals and samples |
| src/kernel/kernel_prof.c | - | s_init_once | once | keep | - | no rerun: profiler init only |
| src/kernel/kernel_prof.c | - | s_entries | file_static | keep | - | kernel profiler totals and samples |
| src/kernel/kernel_prof.c | - | s_used | file_static | keep | - | kernel profiler totals and samples |
| src/kernel/kernel_prof.c | - | s_outside | file_static | keep | - | kernel profiler totals and samples |
| src/kernel/kernel_prof.c | - | s_counters | file_static | keep | - | kernel profiler totals and samples |
| src/kernel/kernel_prof.c | - | s_overflow | file_static | keep | - | kernel profiler totals and samples |
| src/kernel/kernel_prof.c | - | s_lock | file_static | keep | - | profiler host lock |
| src/kernel/kernel_prof.c | - | s_lock_once | once | keep | - | no rerun: profiler init only |
| src/kernel/kernel_prof.c | - | t_current | tls | reset | kernel | in-progress profiler entry of the unwound boot thread |
| src/kernel/kernel_prof.c | xbox_ProfNowUs | freq | local_static | constant | - | host counter frequency |
| src/kernel/kernel_prof.c | - | s_watch_routine | file_static | reset | kernel | watched in-flight kernel call of a thread that exits |
| src/kernel/kernel_prof.c | - | s_watch_t0 | file_static | reset | kernel | watched in-flight kernel call of a thread that exits |
| src/kernel/kernel_prof.c | - | s_watch_esp | file_static | reset | kernel | points at g_esp TLS of an exited thread; dangles |
| src/kernel/kernel_prof.c | - | s_watch_esp0 | file_static | reset | kernel | watched in-flight kernel call of a thread that exits |
| src/kernel/kernel_prof.c | - | s_watch_tid | file_static | reset | kernel | watched in-flight kernel call of a thread that exits |
| src/kernel/kernel_prof.c | - | s_watch_ord | file_static | reset | kernel | watched in-flight kernel call of a thread that exits |
| src/kernel/kernel_prof.c | - | s_samples | file_static | keep | - | kernel profiler totals and samples |
| src/kernel/kernel_prof.c | - | s_samples_total | file_static | keep | - | kernel profiler totals and samples |
| src/kernel/kernel_prof.c | - | s_samples_lost | file_static | keep | - | kernel profiler totals and samples |

### src/kernel/kernel_ramfs.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_ramfs.c | - | s_lock | file_static | keep | - | RAM cache host lock and config |
| src/kernel/kernel_ramfs.c | - | s_enabled | file_static | keep | - | RAM cache host lock and config |
| src/kernel/kernel_ramfs.c | - | s_started | once | reset | saves | rerun: drop store, reseed RAM cache |
| src/kernel/kernel_ramfs.c | - | s_cap | file_static | keep | - | RAM cache host lock and config |
| src/kernel/kernel_ramfs.c | - | s_used | file_static | reset | saves | RAM cache store, nodes and guest file handles; reseeded |
| src/kernel/kernel_ramfs.c | - | s_next_id | file_static | reset | saves | RAM cache store, nodes and guest file handles; reseeded |
| src/kernel/kernel_ramfs.c | - | s_nodes | file_static | reset | saves | RAM cache store, nodes and guest file handles; reseeded |
| src/kernel/kernel_ramfs.c | - | s_node_count | file_static | reset | saves | RAM cache store, nodes and guest file handles; reseeded |
| src/kernel/kernel_ramfs.c | - | s_node_cap | file_static | reset | saves | RAM cache store, nodes and guest file handles; reseeded |
| src/kernel/kernel_ramfs.c | - | s_handles | file_static | reset | saves | RAM cache store, nodes and guest file handles; reseeded |
| src/kernel/kernel_ramfs.c | - | s_handle_count | file_static | reset | saves | RAM cache store, nodes and guest file handles; reseeded |
| src/kernel/kernel_ramfs.c | - | s_handle_cap | file_static | reset | saves | RAM cache store, nodes and guest file handles; reseeded |
| src/kernel/kernel_ramfs.c | - | s_full_warned | file_static | keep | - | warn-once guard |
| src/kernel/kernel_ramfs.c | - | s_seed_overflow | file_static | reset | saves | RAM cache store, nodes and guest file handles; reseeded |
| src/kernel/kernel_ramfs.c | env_says_on | warned | local_static | keep | - | warn-once guard |
| src/kernel/kernel_ramfs.c | - | s_seed_files | file_static | reset | saves | RAM cache store, nodes and guest file handles; reseeded |
| src/kernel/kernel_ramfs.c | - | s_seed_bytes | file_static | reset | saves | RAM cache store, nodes and guest file handles; reseeded |

### src/kernel/kernel_rtl.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_rtl.c | - | g_cs_table | file_static | reset | kernel | shadows of guest critical sections; abandon held ones |
| src/kernel/kernel_rtl.c | - | g_cs_table_lock | file_static | keep | - | host lock for the CS table |
| src/kernel/kernel_rtl.c | - | g_cs_table_full | file_static | reset | kernel | table-full flag for guest CS shadows |
| src/kernel/kernel_rtl.c | - | g_cs_single | file_static | reset | kernel | single recursive lock an exiting thread may hold; replace |
| src/kernel/kernel_rtl.c | - | g_cs_single_once | once | reset | kernel | rerun: new single lock replaces an abandoned one |
| src/kernel/kernel_rtl.c | - | g_cs_single_mode | file_static | keep | - | CS mode from env |
| src/kernel/kernel_rtl.c | - | t_cs_single_depth | tls | reset | kernel | this thread's recursions on the single lock; the guest unwind lets them go and zeroes it |
| src/kernel/kernel_rtl.c | - | g_cs_contention_reports | file_static | keep | - | CS statistics |
| src/kernel/kernel_rtl.c | - | g_cs_enters | file_static | keep | - | CS statistics |
| src/kernel/kernel_rtl.c | - | g_cs_leaves | file_static | keep | - | CS statistics |
| src/kernel/kernel_rtl.c | - | g_crt_lock_table | file_static | constant | - | fixed CRT lock table VA the game declares |
| src/kernel/kernel_rtl.c | - | g_crt_lock_count | file_static | constant | - | fixed CRT lock table VA the game declares |
| src/kernel/kernel_rtl.c | crt_lock_trace | enabled | local_static | keep | - | CRT lock trace env cache |
| src/kernel/kernel_rtl.c | crt_lock_trace | all | local_static | keep | - | CRT lock trace env cache |
| src/kernel/kernel_rtl.c | crt_lock_trace | watch | local_static | keep | - | CRT lock trace env cache |
| src/kernel/kernel_rtl.c | crt_lock_trace | watch_va | local_static | keep | - | CRT lock trace env cache |

### src/kernel/kernel_sync.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_sync.c | - | g_timer_queue | file_static | reset | kernel | host timer queue holding guest KTIMER/DPC callbacks; delete timers |
| src/kernel/kernel_sync.c | - | g_timer_cs | file_static | keep | - | host lock for the timer queue |
| src/kernel/kernel_sync.c | - | g_timer_cs_init | once | keep | - | no rerun: host lock init only |

### src/kernel/kernel_thread.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_thread.c | - | s_guest_pin_failed | file_static | keep | - | host thread pin/priority config and failure guard |
| src/kernel/kernel_thread.c | - | s_guest_priority | file_static | keep | - | host thread pin/priority config and failure guard |
| src/kernel/kernel_thread.c | - | g_game_thread | file_static | reset | kernel | host handle of the spawned game thread |
| src/kernel/kernel_thread.c | - | g_worker_stack_used | file_static | reset | kernel | worker stack slices taken from the guest heap |
| src/kernel/kernel_thread.c | xbox_GuestThreadPin | announced | local_static | keep | - | log-once guards |
| src/kernel/kernel_thread.c | xbox_GuestThreadPin | failed | local_static | keep | - | log-once guards |

### src/kernel/kernel_thunks.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_thunks.c | - | xbox_kernel_thunk_table | global | constant | - | host-native thunk table from XBE ordinals, same each boot |
| src/kernel/kernel_thunks.c | - | g_log_file | file_static | keep | - | kernel log file, level and its lock |
| src/kernel/kernel_thunks.c | - | g_log_level | file_static | keep | - | kernel log file, level and its lock |
| src/kernel/kernel_thunks.c | - | g_log_cs | file_static | keep | - | kernel log file, level and its lock |
| src/kernel/kernel_thunks.c | - | g_log_cs_init | once | keep | - | no rerun: log lock init only |

### src/kernel/kernel_user_apc.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_user_apc.c | - | t_ring | file_static | reset | kernel | per-thread user APC ring with guest routine VAs |
| src/kernel/kernel_user_apc.c | - | t_head | file_static | reset | kernel | per-thread user APC ring with guest routine VAs |
| src/kernel/kernel_user_apc.c | - | t_count | file_static | reset | kernel | per-thread user APC ring with guest routine VAs |
| src/kernel/kernel_user_apc.c | - | t_seq | file_static | reset | kernel | per-thread user APC ring with guest routine VAs |

### src/kernel/kernel_xbox.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_xbox.c | - | xbox_HardwareInfo | global | constant | - | host-native kernel data; bridge uses guest copies instead |
| src/kernel/kernel_xbox.c | - | xbox_KrnlVersion | global | constant | - | host-native kernel data; bridge uses guest copies instead |
| src/kernel/kernel_xbox.c | - | xbox_EEPROMKey | global | constant | - | host-native kernel data; bridge uses guest copies instead |
| src/kernel/kernel_xbox.c | - | xbox_HDKey | global | constant | - | host-native kernel data; bridge uses guest copies instead |
| src/kernel/kernel_xbox.c | - | xbox_SignatureKey | global | constant | - | host-native kernel data; bridge uses guest copies instead |
| src/kernel/kernel_xbox.c | - | xbox_LANKey | global | constant | - | host-native kernel data; bridge uses guest copies instead |
| src/kernel/kernel_xbox.c | - | xbox_AlternateSignatureKeys | global | constant | - | host-native kernel data; bridge uses guest copies instead |
| src/kernel/kernel_xbox.c | - | xbox_XePublicKeyData | global | constant | - | host-native kernel data; bridge uses guest copies instead |
| src/kernel/kernel_xbox.c | - | g_image_filename | file_static | constant | - | host-native kernel data; bridge uses guest copies instead |
| src/kernel/kernel_xbox.c | - | xbox_XeImageFileName | global | constant | - | host-native kernel data; bridge uses guest copies instead |
| src/kernel/kernel_xbox.c | - | g_launch_data_page | file_static | constant | - | host-native kernel data; bridge uses guest copies instead |
| src/kernel/kernel_xbox.c | - | xbox_LaunchDataPage | global | constant | - | host-native kernel data; bridge uses guest copies instead |
| src/kernel/kernel_xbox.c | - | s_tz | file_static | constant | - | host time zone, read once |
| src/kernel/kernel_xbox.c | tz_init | done | local_static | constant | - | time zone read-once guard |
| src/kernel/kernel_xbox.c | xbox_saves_keys_id | id | local_static | constant | - | key id string, the same on every boot |

### src/kernel/recomp_hook.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/recomp_hook.c | - | s_tabs | file_static | keep | - | mod hook chains published at init; code VAs fixed |
| src/kernel/recomp_hook.c | - | s_ntabs | file_static | keep | - | mod hook chains published at init; code VAs fixed |
| src/kernel/recomp_hook.c | - | s_q | file_static | keep | - | mod hook chains published at init; code VAs fixed |
| src/kernel/recomp_hook.c | - | s_nq | file_static | keep | - | mod hook chains published at init; code VAs fixed |
| src/kernel/recomp_hook.c | - | s_chains | file_static | keep | - | mod hook chains published at init; code VAs fixed |
| src/kernel/recomp_hook.c | - | s_published | file_static | keep | - | mod hook chains published at init; code VAs fixed |

### src/kernel/recomp_trace.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/recomp_trace.c | trace_budget | budget | local_static | keep | - | trace line budget |
| src/kernel/recomp_trace.c | - | g_prof | file_static | keep | - | function profiler totals |
| src/kernel/recomp_trace.c | - | g_prof_name | file_static | keep | - | function profiler totals |
| src/kernel/recomp_trace.c | - | g_prof_used | file_static | keep | - | function profiler totals |
| src/kernel/recomp_trace.c | - | g_prof_full | file_static | keep | - | function profiler totals |
| src/kernel/recomp_trace.c | - | g_prof_calls | file_static | keep | - | function profiler totals |
| src/kernel/recomp_trace.c | prof_interval | every | local_static | keep | - | env cache for profile interval |
| src/kernel/recomp_trace.c | prof_enabled | on | local_static | keep | - | env cache |

### src/kernel/xbe_sections.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/xbe_sections.c | - | s_sect | file_static | reset | xbe | section table and resident flags; sections reloaded per boot |
| src/kernel/xbe_sections.c | - | s_n | file_static | reset | xbe | section table and resident flags; sections reloaded per boot |
| src/kernel/xbe_sections.c | - | s_paging | file_static | keep | - | paging mode config and host lock |
| src/kernel/xbe_sections.c | - | s_resident_bytes | file_static | reset | xbe | section table and resident flags; sections reloaded per boot |
| src/kernel/xbe_sections.c | - | s_resident | file_static | reset | xbe | section table and resident flags; sections reloaded per boot |
| src/kernel/xbe_sections.c | - | s_lock | file_static | keep | - | paging mode config and host lock |

### src/kernel/xbox_memory_layout.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/xbox_memory_layout.c | - | g_memory_base | file_static | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_memory_size | file_static | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_memory_offset | file_static | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_xbox_total_ram | global | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_xbox_map_size | global | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_xbox_heap_cap | global | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_mapping_handle | file_static | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_mirror_views | file_static | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_span_base | file_static | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_span_size | file_static | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_span_whole_window | file_static | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_tiled_view | file_static | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_contig_memory | file_static | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_nv2a_memory | file_static | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_mcpx_memory | file_static | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_flash_memory | file_static | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_contig_mapping | file_static | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | s_VirtualAlloc2 | file_static | constant | - | host API pointers resolved from the OS |
| src/kernel/xbox_memory_layout.c | - | s_MapViewOfFile3 | file_static | constant | - | host API pointers resolved from the OS |
| src/kernel/xbox_memory_layout.c | - | g_win_window_base | file_static | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_win_window_size | file_static | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_win_reserve_method | file_static | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_nv2a_ack_thread | file_static | keep | - | nv2a-ack device thread handle and stop flag; thread keeps running |
| src/kernel/xbox_memory_layout.c | - | g_nv2a_ack_stop | file_static | keep | - | nv2a-ack device thread handle and stop flag; thread keeps running |
| src/kernel/xbox_memory_layout.c | - | g_ac97_page | file_static | keep | - | AC97 write-trap page, fault handler and lock; trap stays armed |
| src/kernel/xbox_memory_layout.c | - | g_ac97_veh | file_static | keep | - | AC97 write-trap page, fault handler and lock; trap stays armed |
| src/kernel/xbox_memory_layout.c | - | s_ac97_stepping | tls | keep | - | per-thread single-step state of the AC97 trap handler |
| src/kernel/xbox_memory_layout.c | - | g_ac97_hsize | file_static | keep | - | AC97 write-trap page, fault handler and lock; trap stays armed |
| src/kernel/xbox_memory_layout.c | - | g_ac97_posix_lock | file_static | keep | - | AC97 write-trap page, fault handler and lock; trap stays armed |
| src/kernel/xbox_memory_layout.c | - | g_apu_mmio_trapped | file_static | keep | - | APU MMIO pages stay NOACCESS; memory band must skip them |
| src/kernel/xbox_memory_layout.c | - | g_dsp_ack | file_static | keep | - | RECOMP_DSP_ACK and RECOMP_POKE guest VAs from env |
| src/kernel/xbox_memory_layout.c | - | g_dsp_ack_count | file_static | keep | - | RECOMP_DSP_ACK and RECOMP_POKE guest VAs from env |
| src/kernel/xbox_memory_layout.c | - | g_poke | file_static | keep | - | RECOMP_DSP_ACK and RECOMP_POKE guest VAs from env |
| src/kernel/xbox_memory_layout.c | - | g_poke_count | file_static | keep | - | RECOMP_DSP_ACK and RECOMP_POKE guest VAs from env |
| src/kernel/xbox_memory_layout.c | - | g_mcpx_regs | file_static | keep | - | host pointer to the MCPX register aperture |
| src/kernel/xbox_memory_layout.c | - | g_fence_mirrors | file_static | keep | - | game registrations through fixed device-pointer VAs; not re-registered |
| src/kernel/xbox_memory_layout.c | - | g_fence_mirror_count | file_static | keep | - | game registrations through fixed device-pointer VAs; not re-registered |
| src/kernel/xbox_memory_layout.c | - | g_counter_mirrors | file_static | keep | - | game registrations through fixed device-pointer VAs; not re-registered |
| src/kernel/xbox_memory_layout.c | - | g_counter_mirror_count | file_static | keep | - | game registrations through fixed device-pointer VAs; not re-registered |
| src/kernel/xbox_memory_layout.c | - | g_frame_counters | file_static | reset | video | registrations stay; per-counter bump seed and owner state resets |
| src/kernel/xbox_memory_layout.c | - | g_frame_counter_count | file_static | keep | - | game registrations through fixed device-pointer VAs; not re-registered |
| src/kernel/xbox_memory_layout.c | - | g_frame_counter_last_ms | file_static | keep | - | host ms of last 60 Hz bump and last flip |
| src/kernel/xbox_memory_layout.c | - | g_frame_counters_owned | file_static | reset | video | counters the title moves itself this boot |
| src/kernel/xbox_memory_layout.c | - | g_frame_counter_flip_ms | file_static | keep | - | host ms of last 60 Hz bump and last flip |
| src/kernel/xbox_memory_layout.c | - | s_pb_exec | file_static | keep | - | pushbuffer executor and trace config from env |
| src/kernel/xbox_memory_layout.c | fence_mirrors_tick | last_catchups | local_static | keep | - | fence mirror report statistics and log guard |
| src/kernel/xbox_memory_layout.c | fence_mirrors_tick | missed | local_static | keep | - | fence mirror report statistics and log guard |
| src/kernel/xbox_memory_layout.c | fence_mirrors_tick | said | local_static | keep | - | fence mirror report statistics and log guard |
| src/kernel/xbox_memory_layout.c | fence_completed | said | local_static | keep | - | log-once guard |
| src/kernel/xbox_memory_layout.c | xbox_Nv2aPgraphFenceTick | s_last_mask | local_static | reset | pgraph | vblank interrupt bits last written to guest PMC_INTR |
| src/kernel/xbox_memory_layout.c | - | s_nv2a_trace | file_static | keep | - | pushbuffer executor and trace config from env |
| src/kernel/xbox_memory_layout.c | - | s_pb_report | file_static | keep | - | pushbuffer executor and trace config from env |
| src/kernel/xbox_memory_layout.c | - | s_fb_va | file_static | reset | video | guest framebuffer VA and mode from AvSetDisplayMode |
| src/kernel/xbox_memory_layout.c | - | s_fb_pitch | file_static | reset | video | guest framebuffer VA and mode from AvSetDisplayMode |
| src/kernel/xbox_memory_layout.c | - | s_fb_mode_w | file_static | reset | video | guest framebuffer VA and mode from AvSetDisplayMode |
| src/kernel/xbox_memory_layout.c | - | s_fb_mode_h | file_static | reset | video | guest framebuffer VA and mode from AvSetDisplayMode |
| src/kernel/xbox_memory_layout.c | - | s_fb_format | file_static | reset | video | guest framebuffer VA and mode from AvSetDisplayMode |
| src/kernel/xbox_memory_layout.c | - | s_fb_seq | file_static | keep | - | seqlock counter for the framebuffer record; stays even |
| src/kernel/xbox_memory_layout.c | - | s_display_mode_gen | file_static | reset | video | monotonic display-mode generation; the video hook bumps it at reset, never zeroes it |
| src/kernel/xbox_memory_layout.c | framebuffer_probe_tick | last_ms | local_static | keep | - | framebuffer checksum probe debug state |
| src/kernel/xbox_memory_layout.c | framebuffer_probe_tick | last_sum | local_static | keep | - | framebuffer checksum probe debug state |
| src/kernel/xbox_memory_layout.c | - | g_nv2a_kick_thread | file_static | keep | - | nv2a-kick thread handle and wake object; thread keeps running |
| src/kernel/xbox_memory_layout.c | - | g_kick_fast | file_static | keep | - | kick and run-ahead config from env |
| src/kernel/xbox_memory_layout.c | - | g_kick_seen_ns | file_static | reset | pgraph | time the guest KickOff bit was first seen set |
| src/kernel/xbox_memory_layout.c | - | g_kick_count | file_static | keep | - | kick statistics |
| src/kernel/xbox_memory_layout.c | - | g_scanned_put | file_static | reset | pgraph | DMA_PUT the walker executed to; guest pushbuffer restarts |
| src/kernel/xbox_memory_layout.c | - | g_kick_wait_ns | file_static | keep | - | kick statistics |
| src/kernel/xbox_memory_layout.c | - | g_kick_event | file_static | keep | - | kick and run-ahead config from env |
| src/kernel/xbox_memory_layout.c | - | g_kick_wake | file_static | keep | - | nv2a-kick thread handle and wake object; thread keeps running |
| src/kernel/xbox_memory_layout.c | xbox_Nv2aKickReport | last_cpu | local_static | keep | - | kick report statistics |
| src/kernel/xbox_memory_layout.c | - | g_run_ahead | file_static | keep | - | kick and run-ahead config from env |
| src/kernel/xbox_memory_layout.c | - | g_gr_notify_mask | file_static | keep | - | kick and run-ahead config from env |
| src/kernel/xbox_memory_layout.c | - | g_run_ahead_depth | file_static | keep | - | kick and run-ahead config from env |
| src/kernel/xbox_memory_layout.c | - | g_walk_acks | file_static | reset | pgraph | acks given in the walk in progress |
| src/kernel/xbox_memory_layout.c | - | g_gr | file_static | reset | pgraph | pending and queued PGRAPH notify state |
| src/kernel/xbox_memory_layout.c | - | g_ra | file_static | keep | - | run-ahead trace counters |
| src/kernel/xbox_memory_layout.c | xbox_Nv2aGrNotify | said_queued | local_static | keep | - | log-once guards |
| src/kernel/xbox_memory_layout.c | xbox_Nv2aGrNotify | said_dropped | local_static | keep | - | log-once guards |
| src/kernel/xbox_memory_layout.c | xbox_Nv2aGrNotify | said | local_static | keep | - | log-once guards |
| src/kernel/xbox_memory_layout.c | nv2a_ack_thread | last_put_ms | local_static | keep | - | ack thread report and idle timing |
| src/kernel/xbox_memory_layout.c | nv2a_ack_thread | last_report | local_static | keep | - | ack thread report and idle timing |
| src/kernel/xbox_memory_layout.c | nv2a_ack_thread | wraps | local_static | keep | - | ack thread report and idle timing |
| src/kernel/xbox_memory_layout.c | nv2a_ack_thread | report_ms | local_static | keep | - | ack thread report and idle timing |
| src/kernel/xbox_memory_layout.c | nv2a_ack_thread | last_start | local_static | keep | - | ack thread report and idle timing |
| src/kernel/xbox_memory_layout.c | nv2a_ack_thread | last_active | local_static | keep | - | ack thread report and idle timing |
| src/kernel/xbox_memory_layout.c | - | g_kernel_memory | file_static | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_xbox_mem_offset | global | keep | - | host mapping of the guest window; cleared in place, never unmapped |
| src/kernel/xbox_memory_layout.c | - | g_xbox_image_lo | global | keep | - | XBE image and region geometry; same image reloads |
| src/kernel/xbox_memory_layout.c | - | g_xbox_image_hi | global | keep | - | XBE image and region geometry; same image reloads |
| src/kernel/xbox_memory_layout.c | - | g_xbox_region_base | global | keep | - | XBE image and region geometry; same image reloads |
| src/kernel/xbox_memory_layout.c | - | g_xbox_stack_base | global | keep | - | XBE image and region geometry; same image reloads |
| src/kernel/xbox_memory_layout.c | - | g_xbox_stack_size | global | keep | - | XBE image and region geometry; same image reloads |
| src/kernel/xbox_memory_layout.c | - | g_xbox_code_lo | global | keep | - | XBE image and region geometry; same image reloads |
| src/kernel/xbox_memory_layout.c | - | g_xbox_code_hi | global | keep | - | XBE image and region geometry; same image reloads |
| src/kernel/xbox_memory_layout.c | - | g_fs_base | tls | reset | kernel | thread TIB VA; boot thread returns to XBOX_TIB_MAIN |
| src/kernel/xbox_memory_layout.c | - | g_tls_template_va | file_static | keep | - | TLS template shape from the XBE; band 9 rewrites the block |
| src/kernel/xbox_memory_layout.c | - | g_tls_total | file_static | keep | - | TLS template shape from the XBE; band 9 rewrites the block |
| src/kernel/xbox_memory_layout.c | - | g_tls_thread_size | file_static | keep | - | TLS template shape from the XBE; band 9 rewrites the block |
| src/kernel/xbox_memory_layout.c | - | g_eax | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_ecx | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_edx | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_esp | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_ebx | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_esi | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_edi | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | recomp_abi_violation_log | seen | local_static | keep | - | ABI violation log guards and counts |
| src/kernel/xbox_memory_layout.c | recomp_abi_violation_log | hits | local_static | keep | - | ABI violation log guards and counts |
| src/kernel/xbox_memory_layout.c | recomp_abi_violation_log | count | local_static | keep | - | ABI violation log guards and counts |
| src/kernel/xbox_memory_layout.c | recomp_abi_violation_log | esp_seen | local_static | keep | - | ABI violation log guards and counts |
| src/kernel/xbox_memory_layout.c | recomp_abi_violation_log | esp_count | local_static | keep | - | ABI violation log guards and counts |
| src/kernel/xbox_memory_layout.c | recomp_abi_violation_log | seq | local_static | keep | - | ABI violation log guards and counts |
| src/kernel/xbox_memory_layout.c | - | g_seh_ebp | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_fp_stack | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_fp_top | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_force_return | global | keep | - | force-return switch set once at startup |
| src/kernel/xbox_memory_layout.c | - | g_fp_control_word | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_mxcsr | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_fp_cmp | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_fp_cc | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | s_jmp | tls | reset | kernel | setjmp slots of guest frames the unwind dropped |
| src/kernel/xbox_memory_layout.c | - | s_jmp_used | tls | reset | kernel | setjmp slots of guest frames the unwind dropped |
| src/kernel/xbox_memory_layout.c | - | s_watchdog_esp | file_static | keep | - | watchdog pointers into boot thread TLS, which is reused |
| src/kernel/xbox_memory_layout.c | - | s_watchdog_regs | file_static | keep | - | watchdog pointers into boot thread TLS, which is reused |
| src/kernel/xbox_memory_layout.c | - | s_watchdog_secs | file_static | keep | - | watchdog pointers into boot thread TLS, which is reused |
| src/kernel/xbox_memory_layout.c | - | g_watch_va | file_static | keep | - | RECOMP_WATCH debug watch; stays armed across reset |
| src/kernel/xbox_memory_layout.c | - | g_watch_page | file_static | keep | - | RECOMP_WATCH debug watch; stays armed across reset |
| src/kernel/xbox_memory_layout.c | - | g_watch_last | file_static | keep | - | RECOMP_WATCH debug watch; stays armed across reset |
| src/kernel/xbox_memory_layout.c | - | g_watch_veh | file_static | keep | - | RECOMP_WATCH debug watch; stays armed across reset |
| src/kernel/xbox_memory_layout.c | - | s_watch_stepping | tls | keep | - | RECOMP_WATCH debug watch; stays armed across reset |
| src/kernel/xbox_memory_layout.c | - | g_watch_alias | file_static | keep | - | RECOMP_WATCH debug watch; stays armed across reset |
| src/kernel/xbox_memory_layout.c | - | s_watch_open | file_static | keep | - | RECOMP_WATCH debug watch; stays armed across reset |
| src/kernel/xbox_memory_layout.c | watch_rearm_thread | said | local_static | keep | - | re-arm thread log guards |
| src/kernel/xbox_memory_layout.c | watch_rearm_thread | stuck | local_static | keep | - | re-arm thread log guards |
| src/kernel/xbox_memory_layout.c | - | g_watch_len | file_static | keep | - | RECOMP_WATCH debug watch; stays armed across reset |
| src/kernel/xbox_memory_layout.c | - | g_watch_hpage | file_static | keep | - | RECOMP_WATCH debug watch; stays armed across reset |
| src/kernel/xbox_memory_layout.c | - | g_watch_hsize | file_static | keep | - | RECOMP_WATCH debug watch; stays armed across reset |
| src/kernel/xbox_memory_layout.c | - | g_watch_snap | file_static | keep | - | RECOMP_WATCH debug watch; stays armed across reset |
| src/kernel/xbox_memory_layout.c | - | g_watch_open | file_static | keep | - | RECOMP_WATCH debug watch; stays armed across reset |
| src/kernel/xbox_memory_layout.c | - | g_watch_prev_bus | file_static | keep | - | RECOMP_WATCH debug watch; stays armed across reset |
| src/kernel/xbox_memory_layout.c | - | g_watch_prev_segv | file_static | keep | - | RECOMP_WATCH debug watch; stays armed across reset |
| src/kernel/xbox_memory_layout.c | - | g_watch_derefs | file_static | keep | - | RECOMP_WATCH debug watch; stays armed across reset |
| src/kernel/xbox_memory_layout.c | - | g_watch_root | file_static | keep | - | RECOMP_WATCH debug watch; stays armed across reset |
| src/kernel/xbox_memory_layout.c | - | g_watch_off | file_static | keep | - | RECOMP_WATCH debug watch; stays armed across reset |
| src/kernel/xbox_memory_layout.c | - | g_mm0 | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_mm1 | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_mm2 | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_mm3 | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_mm4 | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_mm5 | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_mm6 | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_mm7 | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_xmm0 | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_xmm1 | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_xmm2 | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_xmm3 | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_xmm4 | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_xmm5 | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_xmm6 | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_xmm7 | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_ebp | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_df | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_eflags | tls | reset | kernel | boot thread guest register; boot loop sets g_esp again |
| src/kernel/xbox_memory_layout.c | - | g_icall_trace | global | keep | - | indirect-call trace ring for crash reports |
| src/kernel/xbox_memory_layout.c | - | g_icall_trace_idx | global | keep | - | indirect-call trace ring for crash reports |
| src/kernel/xbox_memory_layout.c | - | g_icall_count | global | keep | - | indirect-call trace ring for crash reports |
| src/kernel/xbox_memory_layout.c | - | g_reserve_next | file_static | reset | memory | bump pointer for guest address reservations |
| src/kernel/xbox_memory_layout.c | - | g_guest_prot | file_static | reset | memory | guest page protections; restore RW before clearing |
| src/kernel/xbox_memory_layout.c | - | g_host_restrict | file_static | reset | memory | guest page protections; restore RW before clearing |
| src/kernel/xbox_memory_layout.c | - | g_guest_prot_lock | file_static | keep | - | host lock guarding allocator state |
| src/kernel/xbox_memory_layout.c | guest_host_page | page | local_static | constant | - | host page size |
| src/kernel/xbox_memory_layout.c | - | g_heap_next | file_static | reset | memory | guest heap bump pointer and block table |
| src/kernel/xbox_memory_layout.c | - | g_heap_alloc_count | file_static | keep | - | heap allocation count for log budget |
| src/kernel/xbox_memory_layout.c | - | g_heap_blocks | file_static | reset | memory | guest heap bump pointer and block table |
| src/kernel/xbox_memory_layout.c | - | g_heap_block_count | file_static | reset | memory | guest heap bump pointer and block table |
| src/kernel/xbox_memory_layout.c | - | g_heap_lock | file_static | keep | - | host lock guarding allocator state |
| src/kernel/xbox_memory_layout.c | xbox_HeapReclaimEnabled | on | local_static | keep | - | env cache |
| src/kernel/xbox_memory_layout.c | - | g_thread_stacks_used | file_static | reset | memory | thread stacks taken from the guest heap |
| src/kernel/xbox_memory_layout.c | - | g_contig_used | file_static | reset | memory | contiguous page allocator map and high-water mark |
| src/kernel/xbox_memory_layout.c | - | g_contig_len | file_static | reset | memory | contiguous page allocator map and high-water mark |
| src/kernel/xbox_memory_layout.c | - | g_contig_pages_in_use | file_static | reset | memory | contiguous page allocator map and high-water mark |
| src/kernel/xbox_memory_layout.c | - | g_contig_high | file_static | reset | memory | contiguous page allocator map and high-water mark |
| src/kernel/xbox_memory_layout.c | - | g_contig_first_free | file_static | reset | memory | contiguous page allocator map and high-water mark |
| src/kernel/xbox_memory_layout.c | - | g_contig_allocs | file_static | keep | - | contiguous allocator statistics |
| src/kernel/xbox_memory_layout.c | - | g_contig_frees | file_static | keep | - | contiguous allocator statistics |
| src/kernel/xbox_memory_layout.c | - | g_contig_peak_pages | file_static | keep | - | contiguous allocator statistics |
| src/kernel/xbox_memory_layout.c | - | g_contig_t0 | file_static | keep | - | contiguous allocator statistics |
| src/kernel/xbox_memory_layout.c | - | g_contig_lock | file_static | keep | - | host lock guarding allocator state |
| src/kernel/xbox_memory_layout.c | contig_report | last_pages | local_static | keep | - | contiguous report statistics |
| src/kernel/xbox_memory_layout.c | contig_report | ops | local_static | keep | - | contiguous report statistics |
| src/kernel/xbox_memory_layout.c | - | g_phys_image_named | file_static | reset | memory | pages the guest named through MmGetPhysicalAddress |
| src/kernel/xbox_memory_layout.c | contig_avoid_image | on | local_static | keep | - | env cache |
| src/kernel/xbox_memory_layout.c | xbox_ContiguousFree | warned | local_static | keep | - | warn-once guard |
| src/kernel/xbox_memory_layout.c | - | g_phys_ram_named | file_static | reset | memory | pages the guest named through MmGetPhysicalAddress |
| src/kernel/xbox_memory_layout.c | xbox_PhysNoteRamName | reported | local_static | keep | - | log-once guard |
| src/kernel/xbox_memory_layout.c | heap_report_exhaustion | dumped | local_static | keep | - | exhaustion report guard and statistics |
| src/kernel/xbox_memory_layout.c | heap_report_exhaustion | by | local_static | keep | - | exhaustion report guard and statistics |
| src/kernel/xbox_memory_layout.c | heap_free_locked | frees | local_static | keep | - | heap free statistics |
| src/kernel/xbox_memory_layout.c | heap_free_locked | matched | local_static | keep | - | heap free statistics |
| src/kernel/xbox_memory_layout.c | - | g_quar_va | file_static | reset | memory | quarantined guest heap blocks |
| src/kernel/xbox_memory_layout.c | - | g_quar_size | file_static | reset | memory | quarantined guest heap blocks |
| src/kernel/xbox_memory_layout.c | - | g_quar_head | file_static | reset | memory | quarantined guest heap blocks |
| src/kernel/xbox_memory_layout.c | - | g_quar_n | file_static | reset | memory | quarantined guest heap blocks |
| src/kernel/xbox_memory_layout.c | - | g_quar_bytes | file_static | reset | memory | quarantined guest heap blocks |
| src/kernel/xbox_memory_layout.c | heap_quarantine_limit | init | once | keep | - | no rerun: env limit read only |
| src/kernel/xbox_memory_layout.c | heap_quarantine_limit | limit | local_static | keep | - | quarantine limit env cache |
| src/kernel/xbox_memory_layout.c | heap_trace_va | init | once | keep | - | no rerun: env trace VA read only |
| src/kernel/xbox_memory_layout.c | heap_trace_va | va | local_static | keep | - | heap trace VA env cache |

### src/kernel/kernel_reset_probe.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_reset_probe.c | - | g_xbox_reset_probe | global | keep | - | debug stop probe (RECOMP_DEBUG=reset_probe) |
| src/kernel/kernel_reset_probe.c | - | g_xbox_reset_probe_track | global | keep | - | debug stop probe (RECOMP_DEBUG=reset_probe) |
| src/kernel/kernel_reset_probe.c | - | s_busy | file_static | keep | - | debug stop probe (RECOMP_DEBUG=reset_probe) |
| src/kernel/kernel_reset_probe.c | - | s_items | file_static | keep | - | debug stop probe (RECOMP_DEBUG=reset_probe) |
| src/kernel/kernel_reset_probe.c | - | s_kick | file_static | keep | - | debug stop probe (RECOMP_DEBUG=reset_probe) |
| src/kernel/kernel_reset_probe.c | - | s_last | file_static | keep | - | debug stop probe (RECOMP_DEBUG=reset_probe) |
| src/kernel/kernel_reset_probe.c | - | s_last_seq | file_static | keep | - | debug stop probe (RECOMP_DEBUG=reset_probe) |
| src/kernel/kernel_reset_probe.c | - | s_n_items | file_static | keep | - | debug stop probe (RECOMP_DEBUG=reset_probe) |
| src/kernel/kernel_reset_probe.c | - | s_pending_flip | file_static | keep | - | debug stop probe (RECOMP_DEBUG=reset_probe) |
| src/kernel/kernel_reset_probe.c | - | s_slots | file_static | keep | - | debug stop probe (RECOMP_DEBUG=reset_probe) |
| src/kernel/kernel_reset_probe.c | - | s_t0_ns | file_static | keep | - | debug stop probe (RECOMP_DEBUG=reset_probe) |
| src/kernel/kernel_reset_probe.c | - | s_thread_up | file_static | keep | - | debug stop probe (RECOMP_DEBUG=reset_probe) |
| src/kernel/kernel_reset_probe.c | - | s_tracking | file_static | keep | - | debug stop probe (RECOMP_DEBUG=reset_probe) |
| src/kernel/kernel_reset_probe.c | - | t_slot | tls | keep | - | debug stop probe (RECOMP_DEBUG=reset_probe) |

### src/kernel/kernel_guest_unwind.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/kernel_guest_unwind.c | - | g_xbox_reset_pending | global | keep | - | guest unwind: the reset's own flag: up from the request to the resume |
| src/kernel/kernel_guest_unwind.c | - | s_once | once | keep | - | guest unwind: no rerun: the registry lock and condition, made once |
| src/kernel/kernel_guest_unwind.c | - | s_lock | file_static | keep | - | guest unwind: registry lock; never held at a stop point |
| src/kernel/kernel_guest_unwind.c | - | s_cv | file_static | keep | - | guest unwind: registry condition |
| src/kernel/kernel_guest_unwind.c | - | g_xbox_unwind_on | global | keep | - | guest unwind: the unwind is on (the boot loop, a ctest) |
| src/kernel/kernel_guest_unwind.c | - | s_slot | file_static | keep | - | guest unwind: tracked threads; each emptied as its thread parts, the events kept for reuse |
| src/kernel/kernel_guest_unwind.c | - | s_live | file_static | keep | - | guest unwind: tracked thread count; back to the parked boot loop by the reset itself |
| src/kernel/kernel_guest_unwind.c | - | s_starting | file_static | keep | - | guest unwind: threads counted by a creator, not joined yet |
| src/kernel/kernel_guest_unwind.c | - | s_parked | file_static | keep | - | guest unwind: threads waiting at a boot loop |
| src/kernel/kernel_guest_unwind.c | - | s_unwound | file_static | keep | - | guest unwind: stats totals since boot |
| src/kernel/kernel_guest_unwind.c | - | s_refused | file_static | keep | - | guest unwind: stats totals since boot |
| src/kernel/kernel_guest_unwind.c | - | s_cs_released | file_static | keep | - | guest unwind: stats totals since boot |
| src/kernel/kernel_guest_unwind.c | - | s_full_said | file_static | keep | - | guest unwind: log-once guard |
| src/kernel/kernel_guest_unwind.c | - | s_untracked | file_static | keep | - | guest unwind: live threads the registry could not take; a request refuses while it is not zero |
| src/kernel/kernel_guest_unwind.c | - | s_host_calls | file_static | keep | - | guest unwind: host call-ins running guest code; zero when no call-in runs |
| src/kernel/kernel_guest_unwind.c | - | s_foreign | file_static | keep | - | guest unwind: live threads with no call-in; zero whenever a reset may run |
| src/kernel/kernel_guest_unwind.c | - | t_slot1 | tls | keep | - | guest unwind: the thread's registry slot; cleared at its part |
| src/kernel/kernel_guest_unwind.c | - | t_ev | tls | keep | - | guest unwind: the thread's unwind event; cleared at its part |
| src/kernel/kernel_guest_unwind.c | - | t_target | tls | keep | - | guest unwind: the outermost call-in's jmp_buf; cleared by the jump and the part |
| src/kernel/kernel_guest_unwind.c | - | t_hold | tls | keep | - | guest unwind: held host regions; cleared at the part |
| src/kernel/kernel_guest_unwind.c | - | t_untracked | tls | keep | - | guest unwind: the thread is counted as untracked; its part takes it off |

## Host-half inits that write guest memory

Band 9 re-runs these after the memory clear (band 7) and the XBE reload (band 8).

| init | file:line | what it writes | band 9 split (the function to split out) |
|---|---|---|---|
| thunk table patch after VirtualProtect | src/kernel/kernel_bridge.c:12077-12197 | thunk entries -> synthetic VAs / data VAs, plus up to 16 entries below the base; RW then old protect | split the patch loop out of `xbox_kernel_bridge_init` as `bridge_patch_thunks()`; keep `xbox_DpcMarkGuestThread`, `guest_cpu_join`, the slot tables and `xbox_GuestTimeSetStepper` out of it (band 8 reloads .rdata, so the ordinals are back first) |
| kernel_data_init (exported data) | src/kernel/kernel_bridge.c:219-345 (called at 12085) | XboxHardwareInfo, KrnlVersion, KeTickCount, object-type words, KeTimeIncrement, SMC video, IdexChannelObject list head, disk partition count, key arrays at XBOX_KERNEL_DATA_BASE; calls `xbox_GuestStateHashInit` | rerun `kernel_data_init` as is, minus `xbox_GuestStateHashInit` (debug, keep once) |
| launch data page | src/kernel/kernel_bridge.c:248-283 (inside kernel_data_init) | RECOMP_CMDLINE page from `xbox_HeapAlloc` (type 3 + cmdline), its VA in KDATA_LAUNCH_DATA_PAGE | part of the `kernel_data_init` rerun; must run after the heap reset (band 7), since it allocates |
| XboxEEPROMKey | src/kernel/kernel_bridge.c:8325-8335 | 16 zero bytes in the guest heap, cached in `eeprom_va` | no split: zero `eeprom_va` in the kernel hook so the next call allocates again |
| main TIB, PRCB stand-in, fake TLS / RW data, TLS block | src/kernel/xbox_memory_layout.c:4397-4525 | fs:[0..0x28] at XBOX_FS_BASE, zeroed PRCB at region+PRCB_OFF, fake TLS +0x28, TLS block copied from the image's TLS data | split out `layout_init_main_tib()`; run after band 8 (it copies the reloaded TLS init data) |
| boot stack pointer | src/kernel/xbox_memory_layout.c:4393 | `g_esp = XBOX_STACK_TOP` | moves to the boot loop (D6 step 1) |
| worker TIBs and TLS blocks | src/kernel/xbox_memory_layout.c:5496-5538 (`xbox_AllocThreadTib`) | per-thread TIB + TLS block from the guest heap | no split: on demand per thread; the heap reset frees them |
| worker stacks | src/kernel/xbox_memory_layout.c:5541-5568 (`xbox_AllocThreadStack`); src/kernel/kernel_thread.c `g_worker_stack_used` | 512 KB stack slices from the guest heap | no split: reset `g_thread_stacks_used` and `g_worker_stack_used` with the heap |
| XBE init-flag patch | the game's `host_main` (game code, not this group) | clears XINIT_MOUNT_UTILITY_DRIVE in the reloaded XBE header | moves to the boot loop's guest half (D6 step 1); band 8 restores the flag first |
| XBE header copy at 0x10000 and section load | src/kernel/xbox_memory_layout.c:4105-4285 | header, certificate, every section to its VA | band 8 (xbe hook), not band 9; split the section loop out of `xbox_MemoryLayoutInit` |
| fake kernel PE header | src/kernel/xbox_memory_layout.c:4755-4797 | zeroed page at 0x80010000, e_lfanew = 0x80 | split out `layout_init_kernel_page()`; it lies inside the contiguous window, so a contig clear erases it |
| AC97 codec ready bit | src/kernel/xbox_memory_layout.c:4711-4719 | MCPX_AC97_CODEC_STATUS \|= CODEC_READY, then `ac97_arm_write_trap` | rerun the bit set only if band 7 clears the MCPX aperture; the APU hook owns it |
| MMIO trap pages that stay mapped | src/kernel/xbox_memory_layout.c:4708 (APU 0xFE800000+0x30000 NOACCESS), 804 (`ac97_arm_write_trap`, AC97 page read-only) | page protections, not data | no rerun; band 7 must skip the NV2A, MCPX and flash apertures (a memset on the APU trap faults) |
| RECOMP_WATCH re-protects | src/kernel/xbox_memory_layout.c:3200 (`watch_rearm_thread`), 3449 (`watch_posix_arm`), 3545 (`xbox_WatchInit`) | re-protects the watched page every 20 ms | no rerun (debug); band 7 must open the watched page or run with the re-arm paused |
| title page protections | src/kernel/xbox_memory_layout.c:5224-5226 (`g_guest_prot`, `g_host_restrict`) | read-only / no-access pages the title set | before band 7: restore RW on every restricted page, then clear the tables |
| tiled-aperture alias check | src/kernel/xbox_memory_layout.c:4975-4983 | writes A5C30F17 and restores the saved word | no rerun (host check, it restores the word) |

## NV2A walker, pushbuffer executor and shared backend code

### src/kernel/nv2a_backend_common.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/nv2a_backend_common.c | - | s_rt_hashes | file_static | keep | - | render-target hash counter stat |
| src/kernel/nv2a_backend_common.c | - | s_host_opts | file_static | keep | - | enhance options and pending menu requests, host settings |
| src/kernel/nv2a_backend_common.c | - | s_req_opts | file_static | keep | - | enhance options and pending menu requests, host settings |
| src/kernel/nv2a_backend_common.c | - | s_req_mask | file_static | keep | - | enhance options and pending menu requests, host settings |
| src/kernel/nv2a_backend_common.c | - | s_req_lock | file_static | keep | - | host spinlock |
| src/kernel/nv2a_backend_common.c | - | s_req_pending | file_static | keep | - | enhance options and pending menu requests, host settings |
| src/kernel/nv2a_backend_common.c | - | s_opts_gen | file_static | keep | - | options generation; backends cache it, must not go back |
| src/kernel/nv2a_backend_common.c | - | s_hud_state | file_static | reset | enhance | HUD layer active flag and guest target size |
| src/kernel/nv2a_backend_common.c | - | s_frame_wh | file_static | reset | renderer.common | last guest frame size |
| src/kernel/nv2a_backend_common.c | nv2a_bump_trace | seen | local_static | keep | - | trace dedupe list |
| src/kernel/nv2a_backend_common.c | nv2a_bump_trace | seen_n | local_static | keep | - | trace dedupe list |
| src/kernel/nv2a_backend_common.c | - | s_cap | file_static | reset | renderer.common | HUD capture slots posted by guest threads; nv2a_cap_reset |
| src/kernel/nv2a_backend_common.c | - | nv2a_cap_live | global | reset | renderer.common | capture live flag tied to guest slots |
| src/kernel/nv2a_backend_common.c | - | s_cap_flips | file_static | reset | renderer.common | guest flip number of last capture |
| src/kernel/nv2a_backend_common.c | - | s_cap_k | file_static | keep | - | kernel table set once at init; nv2a_cap_reset NULLs it |
| src/kernel/nv2a_backend_common.c | - | nv2a_flip_hook_installed | global | keep | - | flip hook installed by enhance_actions at init |
| src/kernel/nv2a_backend_common.c | - | nv2a_batch_filter_installed | global | keep | - | mod batch filter installed at init |
| src/kernel/nv2a_backend_common.c | nv2a_batch_filter_ask | memo | local_static | reset | renderer.common | memo keyed by prog_dwords/start, not content; unreachable local |
| src/kernel/nv2a_backend_common.c | nv2a_depth_bias | logged | local_static | keep | - | log-once guard |
| src/kernel/nv2a_backend_common.c | nv2a_wdepth_consts_fill | logged | local_static | keep | - | log-once guard |

### src/kernel/nv2a_backend_common.h

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/nv2a_backend_common.h | nv2a_rt_reject_new | set | local_static | keep | - | trace dedupe set per translation unit |

### src/kernel/nv2a_dbg_draw.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/nv2a_dbg_draw.c | - | s_list | file_static | reset | mods | mod debug-draw lists from the old run |
| src/kernel/nv2a_dbg_draw.c | - | s_back | file_static | keep | - | triple-buffer indices shared with flip thread |
| src/kernel/nv2a_dbg_draw.c | - | s_front | file_static | keep | - | triple-buffer indices shared with flip thread |
| src/kernel/nv2a_dbg_draw.c | - | s_mid | file_static | keep | - | triple-buffer indices shared with flip thread |
| src/kernel/nv2a_dbg_draw.c | - | s_any | file_static | reset | mods | debug-draw active flag; nv2a_dbg_hide |
| src/kernel/nv2a_dbg_draw.c | - | s_serial | file_static | keep | - | triple-buffer indices shared with flip thread |

### src/kernel/nv2a_overlay.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/nv2a_overlay.c | - | s_layer | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_armed | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_visible | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_epoch | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_place | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_view_seq | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_view_rect | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_view_out | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_presenter | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_stats_on | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_menu_lock | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_published | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_wake | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_game | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_toast | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_toast_head | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_toast_read | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_shown | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_nshown | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_toast_hook | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_toast_mid | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_img | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_stats_t | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_stats_w | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_stats_h | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_toast_w | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_toast_h | file_static | keep | - | menu and overlay UI state, host |
| src/kernel/nv2a_overlay.c | - | s_rebake | file_static | keep | - | menu and overlay UI state, host |

### src/kernel/nv2a_overlay_font.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/nv2a_overlay_font.c | - | s_font | file_static | keep | - | baked font atlas, host |
| src/kernel/nv2a_overlay_font.c | token_px | px | local_static | constant | - | fixed theme token sizes |

### src/kernel/nv2a_overlay_glyph.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/nv2a_overlay_glyph.c | - | s_namer | file_static | keep | - | glyph namer callback, host UI |

### src/kernel/nv2a_overlay_ui.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/nv2a_overlay_ui.c | - | s_ui_mul | file_static | keep | - | UI scale and display probe, host |
| src/kernel/nv2a_overlay_ui.c | - | s_ui_fit | file_static | keep | - | UI scale and display probe, host |
| src/kernel/nv2a_overlay_ui.c | - | s_disp | file_static | keep | - | UI scale and display probe, host |
| src/kernel/nv2a_overlay_ui.c | - | s_probe | file_static | keep | - | UI scale and display probe, host |
| src/kernel/nv2a_overlay_ui.c | - | s_probed | file_static | keep | - | UI scale and display probe, host |
| src/kernel/nv2a_overlay_ui.c | - | s_comp | file_static | keep | - | UI scale and display probe, host |

### src/kernel/nv2a_pb_exec.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/nv2a_pb_exec.c | - | s_sema_bind | file_static | reset | pgraph | ctxdma bindings read from guest instance memory |
| src/kernel/nv2a_pb_exec.c | - | s_report_bind | file_static | reset | pgraph | ctxdma bindings read from guest instance memory |
| src/kernel/nv2a_pb_exec.c | - | s_2d | file_static | reset | pgraph | 2D engine object binds and NV062/NV09F state set by the guest stream |
| src/kernel/nv2a_pb_exec.c | - | s_2d_src_bind | file_static | reset | pgraph | ctxdma bindings read from guest instance memory |
| src/kernel/nv2a_pb_exec.c | - | s_2d_dst_bind | file_static | reset | pgraph | ctxdma bindings read from guest instance memory |
| src/kernel/nv2a_pb_exec.c | - | s_ramht_slot | file_static | reset | pgraph | RAMHT handle cache: offsets into the guest's hash table |
| src/kernel/nv2a_pb_exec.c | - | s_blits | file_static | keep | - | stats counter |
| src/kernel/nv2a_pb_exec.c | - | s_blits_refused | file_static | keep | - | stats counter |
| src/kernel/nv2a_pb_exec.c | - | s_ramht_scans | file_static | keep | - | stats counter |
| src/kernel/nv2a_pb_exec.c | - | s_ramht_hits | file_static | keep | - | stats counter |
| src/kernel/nv2a_pb_exec.c | twod_refuse | said | local_static | keep | - | log-once guard |
| src/kernel/nv2a_pb_exec.c | twod_blit | said | local_static | keep | - | log-once guard |
| src/kernel/nv2a_pb_exec.c | twod_method | said | local_static | keep | - | log-once guard |
| src/kernel/nv2a_pb_exec.c | surface_write_refused | said | local_static | keep | - | log-once guard |
| src/kernel/nv2a_pb_exec.c | - | s_gpu | file_static | reset | pgraph | PGRAPH register shadow plus counters; keep counters monotonic |
| src/kernel/nv2a_pb_exec.c | - | s_inline_overflow | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_idx_overflow | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_idx_cut | file_static | reset | pgraph | primitive restart index from guest |
| src/kernel/nv2a_pb_exec.c | - | s_idx_max | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_idx_over_ffff | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_xv_fail | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_draw_batches | file_static | reset | pgraph | guest batch setting; restore to 1 |
| src/kernel/nv2a_pb_exec.c | note_idx_overflow | warned | local_static | keep | - | warn-once guard |
| src/kernel/nv2a_pb_exec.c | - | s_sema_releases | file_static | reset | pgraph | semaphore release state, guest addresses |
| src/kernel/nv2a_pb_exec.c | - | s_sema_last_va | file_static | reset | pgraph | semaphore release state, guest addresses |
| src/kernel/nv2a_pb_exec.c | - | s_sema_last_value | file_static | reset | pgraph | semaphore release state, guest addresses |
| src/kernel/nv2a_pb_exec.c | - | s_zpass_count | file_static | reset | pgraph | zpass report state of guest |
| src/kernel/nv2a_pb_exec.c | - | s_zpass_enable | file_static | reset | pgraph | zpass report state of guest |
| src/kernel/nv2a_pb_exec.c | - | t_cnt | file_static | keep | - | thread-local raster counters |
| src/kernel/nv2a_pb_exec.c | - | s_reports | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_reports_zero | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_fence_ref | file_static | reset | pgraph | fence tracking learned from guest stream |
| src/kernel/nv2a_pb_exec.c | - | s_fence_ref_next | file_static | reset | pgraph | fence tracking learned from guest stream |
| src/kernel/nv2a_pb_exec.c | - | s_fence_shift | file_static | reset | pgraph | learned fence shift; restore to -1 |
| src/kernel/nv2a_pb_exec.c | - | s_fence_shift_cand | file_static | reset | pgraph | fence tracking learned from guest stream |
| src/kernel/nv2a_pb_exec.c | - | s_fence_shift_votes | file_static | reset | pgraph | fence tracking learned from guest stream |
| src/kernel/nv2a_pb_exec.c | fence_shift_say | said | local_static | keep | - | log-once guard |
| src/kernel/nv2a_pb_exec.c | - | s_flip_hold | file_static | reset | pgraph | flip hold state; keep the edge flag (mode cached) |
| src/kernel/nv2a_pb_exec.c | - | s_vbl_count | file_static | reset | pgraph | vblank pacing state tied to guest flips |
| src/kernel/nv2a_pb_exec.c | - | s_vbl_moved_ns | file_static | reset | pgraph | vblank pacing state tied to guest flips |
| src/kernel/nv2a_pb_exec.c | - | s_walk_vbl | file_static | reset | pgraph | vblank pacing state tied to guest flips |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_flip_pacing | mode | local_static | keep | - | cached env read; first call sets s_flip_hold.edge |
| src/kernel/nv2a_pb_exec.c | - | s_unhandled | file_static | keep | - | debug report inventory tables |
| src/kernel/nv2a_pb_exec.c | - | s_unhandled_count | file_static | keep | - | debug report inventory tables |
| src/kernel/nv2a_pb_exec.c | - | s_tex_reg | file_static | reset | pgraph | texture stage registers from guest |
| src/kernel/nv2a_pb_exec.c | - | s_tex_set | file_static | reset | pgraph | texture stage registers from guest |
| src/kernel/nv2a_pb_exec.c | - | s_tex_gen | file_static | reset | pgraph | texture state generation; bump, never zero |
| src/kernel/nv2a_pb_exec.c | - | s_tex_use | file_static | keep | - | debug report inventory tables |
| src/kernel/nv2a_pb_exec.c | - | s_tex_use_count | file_static | keep | - | debug report inventory tables |
| src/kernel/nv2a_pb_exec.c | note_texture_use | every | local_static | keep | - | cached env read |
| src/kernel/nv2a_pb_exec.c | note_texture_use | binds | local_static | keep | - | debug log counter |
| src/kernel/nv2a_pb_exec.c | note_texture_use | seq | local_static | keep | - | debug log sequence |
| src/kernel/nv2a_pb_exec.c | - | s_unhandled_slot | file_static | keep | - | debug report inventory tables |
| src/kernel/nv2a_pb_exec.c | present_stale_flips | n | local_static | keep | - | debug counter/cached env |
| src/kernel/nv2a_pb_exec.c | - | s_psurf | file_static | reset | pgraph | present surface picks, guest offsets |
| src/kernel/nv2a_pb_exec.c | - | s_psurf_seq | file_static | reset | pgraph | present surface picks, guest offsets |
| src/kernel/nv2a_pb_exec.c | - | s_psurf_flip_seq | file_static | reset | pgraph | present surface picks, guest offsets |
| src/kernel/nv2a_pb_exec.c | - | s_present | file_static | reset | pgraph | present surface picks, guest offsets |
| src/kernel/nv2a_pb_exec.c | - | s_scanout | file_static | reset | pgraph | present surface picks, guest offsets |
| src/kernel/nv2a_pb_exec.c | - | s_scanout_gen | file_static | reset | pgraph | scanout generation; bump on reset |
| src/kernel/nv2a_pb_exec.c | - | s_present_want | file_static | reset | pgraph | present surface picks, guest offsets |
| src/kernel/nv2a_pb_exec.c | - | s_ptrace | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_ptrace_n | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_ptrace_on | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_evict_n | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_evict_spared | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_evict_protected | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | present_trace_summary | done | local_static | keep | - | log-once guard |
| src/kernel/nv2a_pb_exec.c | - | s_opts_at | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_opts_at_n | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | present_pick | warned_field | local_static | keep | - | warn-once guard |
| src/kernel/nv2a_pb_exec.c | present_pick | s_pitch_seen | local_static | reset | pgraph | video mode detection from guest pitch and flips |
| src/kernel/nv2a_pb_exec.c | present_pick | s_mode_gen | local_static | reset | pgraph | video mode detection from guest pitch and flips |
| src/kernel/nv2a_pb_exec.c | present_pick | s_mode_flip | local_static | reset | pgraph | video mode detection from guest pitch and flips |
| src/kernel/nv2a_pb_exec.c | present_pick | pick | local_static | keep | - | cached env read |
| src/kernel/nv2a_pb_exec.c | - | s_bmp_seq | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_dump_at_n | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_dump_at_lo | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_dump_at_hi | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_fbp | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | fb_prog_keep | warned | local_static | keep | - | warn-once guard |
| src/kernel/nv2a_pb_exec.c | dump_at_flip | checked | local_static | keep | - | debug trigger guard |
| src/kernel/nv2a_pb_exec.c | dump_at_flip | here | local_static | keep | - | debug trigger guard |
| src/kernel/nv2a_pb_exec.c | - | s_surf_wr | file_static | reset | renderer.common | surface write tracking keyed by guest offset; with s_tc |
| src/kernel/nv2a_pb_exec.c | - | s_surf_wr_seq | file_static | reset | renderer.common | surface write tracking keyed by guest offset; with s_tc |
| src/kernel/nv2a_pb_exec.c | surface_swizzle_index | key | local_static | constant | - | memo keyed by format value |
| src/kernel/nv2a_pb_exec.c | surface_swizzle_index | mx | local_static | constant | - | memo keyed by format value |
| src/kernel/nv2a_pb_exec.c | surface_swizzle_index | my | local_static | constant | - | memo keyed by format value |
| src/kernel/nv2a_pb_exec.c | clear_surface | seen | local_static | keep | - | debug log dedupe |
| src/kernel/nv2a_pb_exec.c | clear_surface | n | local_static | keep | - | debug counter/cached env |
| src/kernel/nv2a_pb_exec.c | clear_surface | announced | local_static | keep | - | log-once guard |
| src/kernel/nv2a_pb_exec.c | - | s_tex_fail | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_surface | file_static | reset | pgraph | surface registers with host pointer into guest memory |
| src/kernel/nv2a_pb_exec.c | - | s_drawn_dumps | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_vsh | file_static | reset | pgraph | vertex shader program and constants from guest |
| src/kernel/nv2a_pb_exec.c | - | s_vsh_cgen | file_static | reset | pgraph | vsh constant generation; bump, never zero |
| src/kernel/nv2a_pb_exec.c | - | s_vsh_progs | file_static | keep | - | debug report inventory tables |
| src/kernel/nv2a_pb_exec.c | - | s_vsh_prog_count | file_static | keep | - | debug report inventory tables |
| src/kernel/nv2a_pb_exec.c | - | s_vsh_ab_batches | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_vsh_ab_diff_batches | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_vsh_ab_diff_px | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_vsh_last_d0 | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_vsh_last_v0 | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_zbuf | file_static | reset | renderer.common | CPU depth/stencil buffers of guest surface |
| src/kernel/nv2a_pb_exec.c | - | s_sbuf | file_static | reset | renderer.common | CPU depth/stencil buffers of guest surface |
| src/kernel/nv2a_pb_exec.c | - | s_zbuf_w | file_static | reset | renderer.common | CPU depth/stencil buffers of guest surface |
| src/kernel/nv2a_pb_exec.c | - | s_zbuf_h | file_static | reset | renderer.common | CPU depth/stencil buffers of guest surface |
| src/kernel/nv2a_pb_exec.c | vsh_enabled | on | local_static | keep | - | cached env read |
| src/kernel/nv2a_pb_exec.c | - | s_not_vsh | file_static | constant | - | table depends on method number alone |
| src/kernel/nv2a_pb_exec.c | - | s_ff_active | file_static | reset | pgraph | fixed-function state from guest |
| src/kernel/nv2a_pb_exec.c | ff_enabled | on | local_static | keep | - | cached env read |
| src/kernel/nv2a_pb_exec.c | - | s_ff_cache | file_static | keep | - | fixed-function cache keyed by content |
| src/kernel/nv2a_pb_exec.c | - | s_ff_cache_n | file_static | keep | - | fixed-function cache keyed by content |
| src/kernel/nv2a_pb_exec.c | - | s_ff_cache_next | file_static | keep | - | fixed-function cache keyed by content |
| src/kernel/nv2a_pb_exec.c | - | s_ff_serial | file_static | keep | - | backends memoise shaders on it; never reset |
| src/kernel/nv2a_pb_exec.c | - | s_ff_save | file_static | reset | pgraph | fixed-function state from guest |
| src/kernel/nv2a_pb_exec.c | vsh_note_program | said | local_static | keep | - | log-once guard |
| src/kernel/nv2a_pb_exec.c | - | s_zbc | file_static | reset | renderer.common | depth slots keyed by guest zeta offset |
| src/kernel/nv2a_pb_exec.c | - | s_zb_z | file_static | reset | renderer.common | depth slots keyed by guest zeta offset |
| src/kernel/nv2a_pb_exec.c | - | s_zb_s | file_static | reset | renderer.common | depth slots keyed by guest zeta offset |
| src/kernel/nv2a_pb_exec.c | zb_slots | n | local_static | keep | - | debug counter/cached env |
| src/kernel/nv2a_pb_exec.c | - | s_rt | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_perf | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | perf_draw_on | on | local_static | keep | - | cached env read |
| src/kernel/nv2a_pb_exec.c | - | s_stage_tex | file_static | reset | pgraph | per-stage texture binding from guest |
| src/kernel/nv2a_pb_exec.c | - | s_stage_mode | file_static | reset | pgraph | per-stage texture binding from guest |
| src/kernel/nv2a_pb_exec.c | - | s_stage_scale | file_static | reset | pgraph | per-stage texture binding from guest |
| src/kernel/nv2a_pb_exec.c | - | s_stage_tent | file_static | reset | pgraph | per-stage texture binding from guest |
| src/kernel/nv2a_pb_exec.c | - | s_stage_mip | file_static | reset | pgraph | per-stage texture binding from guest |
| src/kernel/nv2a_pb_exec.c | - | s_tex_log | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_tex_log_count | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_tex_log_overflow | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | tex_log_on | on | local_static | keep | - | cached env read |
| src/kernel/nv2a_pb_exec.c | - | s_surf_log | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_surf_log_count | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | pb_bilinear | on | local_static | keep | - | cached env read |
| src/kernel/nv2a_pb_exec.c | pb_mips | on | local_static | keep | - | cached env read |
| src/kernel/nv2a_pb_exec.c | - | s_rc | file_static | reset | pgraph | register combiner state from guest |
| src/kernel/nv2a_pb_exec.c | vsh_rc_active | on | local_static | keep | - | cached env read |
| src/kernel/nv2a_pb_exec.c | - | s_fast_force | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | pb_fast | on | local_static | keep | - | cached env read |
| src/kernel/nv2a_pb_exec.c | - | s_u8f | file_static | constant | - | fixed u8-to-float table |
| src/kernel/nv2a_pb_exec.c | fast_init_tables | done | local_static | constant | - | fixed fast-path tables built once |
| src/kernel/nv2a_pb_exec.c | - | s_tc | file_static | reset | renderer.common | texture cache keyed by guest offset |
| src/kernel/nv2a_pb_exec.c | - | s_stage_tc | file_static | reset | renderer.common | texture cache keyed by guest offset |
| src/kernel/nv2a_pb_exec.c | - | s_fast_serial | file_static | reset | pgraph | fast-path state serial tied to guest state |
| src/kernel/nv2a_pb_exec.c | - | s_tc_clock | file_static | reset | renderer.common | texture cache keyed by guest offset |
| src/kernel/nv2a_pb_exec.c | - | s_tc_tiles_filled | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_tc_refp | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_tc_dropped | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_tc_binds | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_tc_builds | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_tc_feedback | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_tc_lock | file_static | keep | - | host lock |
| src/kernel/nv2a_pb_exec.c | - | s_rcp | file_static | reset | pgraph | register combiner state from guest |
| src/kernel/nv2a_pb_exec.c | - | s_pool | file_static | keep | - | raster worker pool, host threads |
| src/kernel/nv2a_pb_exec.c | raster_pool_size | init | once | keep | - | no rerun: raster worker threads persist |
| src/kernel/nv2a_pb_exec.c | - | s_clip_tris | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_clip_out | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_clip_gone | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_clip_unusable | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_clip_why | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_clip_origin | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | clip_enabled | on | local_static | keep | - | cached env read |
| src/kernel/nv2a_pb_exec.c | clip_tri | buf | local_static | keep | - | A/B scratch buffer, rewritten each check |
| src/kernel/nv2a_pb_exec.c | clip_tri | shown | local_static | keep | - | log-once guard |
| src/kernel/nv2a_pb_exec.c | raster_batch_vsh_one | xv | local_static | keep | - | A/B scratch buffer, rewritten each check |
| src/kernel/nv2a_pb_exec.c | raster_batch_vsh_one | xv_cap | local_static | keep | - | A/B scratch buffer, rewritten each check |
| src/kernel/nv2a_pb_exec.c | raster_batch_vsh_one | shown3d | local_static | keep | - | log-once guard |
| src/kernel/nv2a_pb_exec.c | raster_batch_vsh_one | shown | local_static | keep | - | log-once guard |
| src/kernel/nv2a_pb_exec.c | - | s_fab_batches | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_fab_bad | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_fab_bad_px | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_fab_bad_z | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_fab_bad_ctr | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | raster_batch_fast_ab | save | local_static | keep | - | A/B scratch buffer, rewritten each check |
| src/kernel/nv2a_pb_exec.c | raster_batch_fast_ab | ref | local_static | keep | - | A/B scratch buffer, rewritten each check |
| src/kernel/nv2a_pb_exec.c | raster_batch_fast_ab | zsave | local_static | keep | - | A/B scratch buffer, rewritten each check |
| src/kernel/nv2a_pb_exec.c | raster_batch_fast_ab | zref | local_static | keep | - | A/B scratch buffer, rewritten each check |
| src/kernel/nv2a_pb_exec.c | raster_batch_fast_ab | ssave | local_static | keep | - | A/B scratch buffer, rewritten each check |
| src/kernel/nv2a_pb_exec.c | raster_batch_fast_ab | sref | local_static | keep | - | A/B scratch buffer, rewritten each check |
| src/kernel/nv2a_pb_exec.c | raster_batch_fast_ab | cap | local_static | keep | - | A/B scratch buffer, rewritten each check |
| src/kernel/nv2a_pb_exec.c | raster_batch_fast_ab | zcap | local_static | keep | - | A/B scratch buffer, rewritten each check |
| src/kernel/nv2a_pb_exec.c | raster_batch_fast_ab | scap | local_static | keep | - | A/B scratch buffer, rewritten each check |
| src/kernel/nv2a_pb_exec.c | raster_batch_fast_ab | gpu0 | local_static | keep | - | A/B scratch buffer, rewritten each check |
| src/kernel/nv2a_pb_exec.c | raster_batch_fast_ab | gpu_ref | local_static | keep | - | A/B scratch buffer, rewritten each check |
| src/kernel/nv2a_pb_exec.c | raster_batch_fast_ab | vsh0 | local_static | keep | - | A/B scratch buffer, rewritten each check |
| src/kernel/nv2a_pb_exec.c | raster_batch_fast_ab | vsh_ref | local_static | keep | - | A/B scratch buffer, rewritten each check |
| src/kernel/nv2a_pb_exec.c | raster_batch_vsh | ab | local_static | keep | - | cached env read for A/B check |
| src/kernel/nv2a_pb_exec.c | - | s_vsh_ab_inner | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | raster_batch_ab | save | local_static | keep | - | A/B scratch buffer, rewritten each check |
| src/kernel/nv2a_pb_exec.c | raster_batch_ab | old | local_static | keep | - | A/B scratch buffer, rewritten each check |
| src/kernel/nv2a_pb_exec.c | raster_batch_ab | cap | local_static | keep | - | A/B scratch buffer, rewritten each check |
| src/kernel/nv2a_pb_exec.c | raster_batch_ab | seen | local_static | keep | - | debug log dedupe |
| src/kernel/nv2a_pb_exec.c | raster_batch_ab | nseen | local_static | keep | - | debug log dedupe |
| src/kernel/nv2a_pb_exec.c | raster_batch_ab | zdumps | local_static | keep | - | debug dump counter |
| src/kernel/nv2a_pb_exec.c | raster_batch_ab | zshown | local_static | keep | - | log-once guard |
| src/kernel/nv2a_pb_exec.c | raster_batch | ab | local_static | keep | - | cached env read for A/B check |
| src/kernel/nv2a_pb_exec.c | - | s_backend | file_static | constant | - | backend function table set at init |
| src/kernel/nv2a_pb_exec.c | - | s_report_owed | file_static | reset | pgraph | pending guest report writes |
| src/kernel/nv2a_pb_exec.c | get_report | fixed_set | local_static | keep | - | cached env override |
| src/kernel/nv2a_pb_exec.c | get_report | fixed | local_static | keep | - | cached env override |
| src/kernel/nv2a_pb_exec.c | - | s_gpu_backend_batches | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_pacing_backend_batches | file_static | keep | - | stats totals |
| src/kernel/nv2a_pb_exec.c | - | s_null_draw_us | file_static | keep | - | null-backend timing setting |
| src/kernel/nv2a_pb_exec.c | - | s_null_backend | file_static | constant | - | backend function table set at init |
| src/kernel/nv2a_pb_exec.c | - | s_cpx_n | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_cpx_x | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_cpx_y | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_cpx_max | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_cpx_f0 | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_cpx_f1 | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | - | s_vcd | file_static | keep | - | debug trace and dump state |
| src/kernel/nv2a_pb_exec.c | vsh_note_gpu | on | local_static | keep | - | cached env read |
| src/kernel/nv2a_pb_exec.c | draw_primitive | shown | local_static | keep | - | log-once guard |
| src/kernel/nv2a_pb_exec.c | - | s_mrg | file_static | reset | pgraph | draw merge carry-over across methods |
| src/kernel/nv2a_pb_exec.c | - | s_batch_pure | file_static | reset | pgraph | draw merge carry-over across methods |
| src/kernel/nv2a_pb_exec.c | - | s_mrg_save_n | file_static | reset | pgraph | draw merge carry-over across methods |
| src/kernel/nv2a_pb_exec.c | - | s_mrg_save | file_static | reset | pgraph | draw merge carry-over across methods |
| src/kernel/nv2a_pb_exec.c | merge_on | env | local_static | keep | - | cached env read |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | inited | once | reset | pgraph | rerun: s_gpu min/max, s_vsh.inl w=1; split out backend registration |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | mlog_flip | local_static | keep | - | cached env read |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | verbose | local_static | keep | - | cached env read |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | shown | local_static | keep | - | log-once guard |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | from | local_static | keep | - | cached env read |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | lines | local_static | keep | - | log line counter |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | said | local_static | keep | - | log-once guard |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | prev | local_static | reset | pgraph | delta bases of s_gpu counters; wrap if s_gpu zeroed |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | last_draws | local_static | reset | pgraph | delta bases of s_gpu counters; wrap if s_gpu zeroed |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | every | local_static | keep | - | cached env read |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | dumped | local_static | keep | - | flip log guard |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | prev_tris | local_static | keep | - | flip log delta base, debug |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | on | local_static | keep | - | cached env read |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | prev_b | local_static | keep | - | flip log delta base, debug |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | prev_t | local_static | keep | - | flip log delta base, debug |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | prev_px | local_static | keep | - | flip log delta base, debug |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | prev_zr | local_static | keep | - | flip log delta base, debug |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | prev_clip | local_static | keep | - | flip log delta base, debug |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | prev_drop | local_static | keep | - | flip log delta base, debug |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | prev_stops | local_static | keep | - | flip log delta base, debug |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_method | n | local_static | keep | - | debug counter/cached env |
| src/kernel/nv2a_pb_exec.c | nv2a_pb_exec_report | done | local_static | keep | - | log-once guard |

### src/kernel/nv2a_pb_scan.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/nv2a_pb_scan.c | - | s_seen | file_static | keep | - | method survey inventory, debug report |
| src/kernel/nv2a_pb_scan.c | - | s_seen_count | file_static | keep | - | method survey inventory, debug report |
| src/kernel/nv2a_pb_scan.c | - | s_tot_words | file_static | keep | - | walker stats totals |
| src/kernel/nv2a_pb_scan.c | - | s_tot_unknown | file_static | keep | - | walker stats totals |
| src/kernel/nv2a_pb_scan.c | - | s_tot_jumps | file_static | keep | - | walker stats totals |
| src/kernel/nv2a_pb_scan.c | - | s_tot_segments | file_static | keep | - | walker stats totals |
| src/kernel/nv2a_pb_scan.c | - | s_tot_stops | file_static | keep | - | walker stats totals |
| src/kernel/nv2a_pb_scan.c | - | s_tot_salvaged | file_static | keep | - | walker stats totals |
| src/kernel/nv2a_pb_scan.c | - | s_catchups | file_static | keep | - | walker stats totals |
| src/kernel/nv2a_pb_scan.c | - | s_exec_enabled | file_static | keep | - | cached env reads |
| src/kernel/nv2a_pb_scan.c | - | s_scan_enabled | file_static | keep | - | cached env reads |
| src/kernel/nv2a_pb_scan.c | - | s_seen_slot | file_static | keep | - | method survey inventory, debug report |
| src/kernel/nv2a_pb_scan.c | note | none | local_static | keep | - | warn-once guards |
| src/kernel/nv2a_pb_scan.c | note | warned | local_static | keep | - | warn-once guards |
| src/kernel/nv2a_pb_scan.c | - | s_left | file_static | reset | pgraph | method header carried over between walks |
| src/kernel/nv2a_pb_scan.c | - | s_subch | file_static | reset | pgraph | method header carried over between walks |
| src/kernel/nv2a_pb_scan.c | - | s_method | file_static | reset | pgraph | method header carried over between walks |
| src/kernel/nv2a_pb_scan.c | - | s_noninc | file_static | reset | pgraph | method header carried over between walks |
| src/kernel/nv2a_pb_scan.c | - | s_in_call | file_static | reset | pgraph | pushbuffer CALL nesting state of guest stream |
| src/kernel/nv2a_pb_scan.c | - | s_checkpoint | file_static | keep | - | run_ahead callback set at init |
| src/kernel/nv2a_pb_scan.c | nv2a_pb_scan | inject | local_static | keep | - | debug inject env and log-once |
| src/kernel/nv2a_pb_scan.c | nv2a_pb_scan | said | local_static | keep | - | debug inject env and log-once |

### src/kernel/nv2a_present_chain.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/nv2a_present_chain.c | - | s_lock | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_req | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_lib_path | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_gen | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_wanted | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_lines_mode | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_req_slug | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_stages | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_nstages | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_profile_err | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_pending | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_prepare | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_lrs | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_lib_state | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_lib_why | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_load_lock | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_host | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_mod | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | nv2a_present_chain_rate | last | local_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | nv2a_present_chain_rate | avg_ns | local_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_params | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_nparams | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_params_key | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_params_scope | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_pgen | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_ovr | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | - | s_novr | file_static | keep | - | host present shader chain, params and library |
| src/kernel/nv2a_present_chain.c | ovr_add | s_full_said | local_static | keep | - | host present shader chain, params and library |

### src/kernel/nv2a_stats.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/nv2a_stats.c | - | nv2a_stats_on | global | keep | - | stats strip counters and ring |
| src/kernel/nv2a_stats.c | - | nv2a_stats_count_ | global | keep | - | stats strip counters and ring |
| src/kernel/nv2a_stats.c | - | s_users | file_static | keep | - | stats strip counters and ring |
| src/kernel/nv2a_stats.c | - | s_ring_us | file_static | keep | - | stats strip counters and ring |
| src/kernel/nv2a_stats.c | - | s_ring_end_ms | file_static | keep | - | stats strip counters and ring |
| src/kernel/nv2a_stats.c | - | s_ring_n | file_static | keep | - | stats strip counters and ring |
| src/kernel/nv2a_stats.c | - | s_last_flip_ns | file_static | keep | - | stats strip counters and ring |

### src/kernel/nv2a_tex_dump.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/nv2a_tex_dump.c | - | s_on | file_static | keep | - | debug dump catalog, content-hash keyed |
| src/kernel/nv2a_tex_dump.c | - | s_dir | file_static | keep | - | debug dump catalog, content-hash keyed |
| src/kernel/nv2a_tex_dump.c | - | s_flip | file_static | keep | - | debug dump catalog, content-hash keyed |
| src/kernel/nv2a_tex_dump.c | - | s_st | file_static | keep | - | debug dump catalog, content-hash keyed |
| src/kernel/nv2a_tex_dump.c | - | s_dirty | file_static | keep | - | debug dump catalog, content-hash keyed |
| src/kernel/nv2a_tex_dump.c | - | s_lock | file_static | keep | - | debug dump catalog, content-hash keyed |
| src/kernel/nv2a_tex_dump.c | - | s_max_bytes | file_static | keep | - | debug dump catalog, content-hash keyed |
| src/kernel/nv2a_tex_dump.c | - | s_stop | file_static | keep | - | debug dump catalog, content-hash keyed |
| src/kernel/nv2a_tex_dump.c | - | s_hash | file_static | keep | - | debug dump catalog, content-hash keyed |
| src/kernel/nv2a_tex_dump.c | - | s_hash_cap | file_static | keep | - | debug dump catalog, content-hash keyed |
| src/kernel/nv2a_tex_dump.c | - | s_hash_n | file_static | keep | - | debug dump catalog, content-hash keyed |
| src/kernel/nv2a_tex_dump.c | - | s_addr | file_static | keep | - | debug dump catalog, content-hash keyed |
| src/kernel/nv2a_tex_dump.c | - | s_addr_cap | file_static | keep | - | debug dump catalog, content-hash keyed |
| src/kernel/nv2a_tex_dump.c | - | s_addr_n | file_static | keep | - | debug dump catalog, content-hash keyed |
| src/kernel/nv2a_tex_dump.c | - | s_rt | file_static | keep | - | debug dump catalog, content-hash keyed |
| src/kernel/nv2a_tex_dump.c | - | s_rt_n | file_static | keep | - | debug dump catalog, content-hash keyed |
| src/kernel/nv2a_tex_dump.c | hook_exit_once | hooked | local_static | keep | - | exit hook registered once |
| src/kernel/nv2a_tex_dump.c | nv2a_tex_dump_upload | why | local_static | keep | - | stop-reason text for log |
| src/kernel/nv2a_tex_dump.c | - | s_mag | file_static | keep | - | the tex_mag trace, a debug figure keyed by content hash |
| src/kernel/nv2a_tex_dump.c | - | s_mag_tab | file_static | keep | - | the tex_mag trace, a debug figure keyed by content hash |
| src/kernel/nv2a_tex_dump.c | - | s_mag_cap | file_static | keep | - | the tex_mag trace, a debug figure keyed by content hash |
| src/kernel/nv2a_tex_dump.c | - | s_mag_n | file_static | keep | - | the tex_mag trace, a debug figure keyed by content hash |
| src/kernel/nv2a_tex_dump.c | - | s_mag_dropped | file_static | keep | - | the tex_mag trace, a debug figure keyed by content hash |

### src/kernel/nv2a_tex_pack.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/kernel/nv2a_tex_pack.c | - | s_entries | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_count | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_scale | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_text_scale | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_kind_count | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_bc3_count | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_path | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_hits | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_lru_head | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_lru_tail | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_resident | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_resident_max | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_freed | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_file_size | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_file_mtime | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_file_changed | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_mask | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_gen | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_miss | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_miss_n | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_miss_next | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_miss_total | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_snap_want | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_snap_lock | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_snap_n | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_snap | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | read_failed | said | local_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_version | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_bc1_count | file_static | keep | - | texture pack keyed by content hash |
| src/kernel/nv2a_tex_pack.c | - | s_keep | file_static | keep | - | texture pack keyed by content hash |

### src/nv2a/nv2a_core.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/nv2a/nv2a_core.c | - | g_nv2a | file_static | reset | pgraph | NV2A register state incl PVIDEO read by fb_present |
| src/nv2a/nv2a_core.c | - | g_vram_region | file_static | keep | - | host mapping of VRAM/RAMIN regions |
| src/nv2a/nv2a_core.c | - | g_ramin_region | file_static | keep | - | host mapping of VRAM/RAMIN regions |
| src/nv2a/nv2a_core.c | - | g_pgraph_method_count | file_static | keep | - | stats counters |
| src/nv2a/nv2a_core.c | - | g_pgraph_draw_count | file_static | keep | - | stats counters |
| src/nv2a/nv2a_core.c | - | g_pgraph_clear_count | file_static | keep | - | stats counters |
| src/nv2a/nv2a_core.c | - | g_pgraph_flip_count | file_static | keep | - | stats counters |
| src/nv2a/nv2a_core.c | - | g_pgraph_inline_verts | file_static | keep | - | stats counters |
| src/nv2a/nv2a_core.c | - | g_pgraph_in_begin | file_static | reset | pgraph | inside guest BEGIN/END pair |

### src/nv2a/nv2a_mmio_hook.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/nv2a/nv2a_mmio_hook.c | - | g_mem_offset | file_static | keep | - | host allocation; library unused by our games |
| src/nv2a/nv2a_mmio_hook.c | - | g_nv2a_vram | file_static | keep | - | host allocation; library unused by our games |
| src/nv2a/nv2a_mmio_hook.c | - | g_mmio_read_count | file_static | keep | - | stats counters |
| src/nv2a/nv2a_mmio_hook.c | - | g_mmio_write_count | file_static | keep | - | stats counters |
| src/nv2a/nv2a_mmio_hook.c | - | g_mmio_decode_fail | file_static | keep | - | stats counters |
| src/nv2a/nv2a_mmio_hook.c | - | g_pvideo_lock | file_static | keep | - | host lock |

### src/nv2a/nv2a_pb_replay.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/nv2a/nv2a_pb_replay.c | - | menu_names | file_static | constant | - | fixed replay tables |
| src/nv2a/nv2a_pb_replay.c | - | menu_pb_data | file_static | constant | - | fixed replay tables |
| src/nv2a/nv2a_pb_replay.c | - | menu_pb_dwords | file_static | constant | - | fixed replay tables |
| src/nv2a/nv2a_pb_replay.c | - | g_replay_active | file_static | reset | pgraph | replay progress of the running session |
| src/nv2a/nv2a_pb_replay.c | - | g_replay_frame | file_static | reset | pgraph | replay progress of the running session |
| src/nv2a/nv2a_pb_replay.c | - | g_current_menu | file_static | reset | pgraph | replay progress of the running session |
| src/nv2a/nv2a_pb_replay.c | - | g_prev_menu | file_static | reset | pgraph | replay progress of the running session |
| src/nv2a/nv2a_pb_replay.c | - | g_replay_start_time | file_static | reset | pgraph | replay progress of the running session |
| src/nv2a/nv2a_pb_replay.c | - | g_replay_freq | file_static | constant | - | host timer frequency |
| src/nv2a/nv2a_pb_replay.c | - | main_items | file_static | constant | - | fixed replay tables |
| src/nv2a/nv2a_pb_replay.c | - | single_items | file_static | constant | - | fixed replay tables |
| src/nv2a/nv2a_pb_replay.c | nv2a_pb_replay_frame | prev_menu | local_static | keep | - | window-title change detection |
| src/nv2a/nv2a_pb_replay.c | nv2a_pb_replay_frame | prev_cursor | local_static | keep | - | window-title change detection |

### src/nv2a/nv2a_pb_test.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/nv2a/nv2a_pb_test.c | - | g_pb_test_frame | file_static | keep | - | host test pattern state |
| src/nv2a/nv2a_pb_test.c | - | g_pb_test_active | file_static | keep | - | host test pattern state |

### src/nv2a/nv2a_pgraph_d3d11.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/nv2a/nv2a_pgraph_d3d11.c | - | g_textures_loaded | file_static | constant | - | fixed textures loaded once |
| src/nv2a/nv2a_pgraph_d3d11.c | - | g_pg | file_static | reset | pgraph | PGRAPH translator state from guest stream |

## HLE D3D8, translators, shader cache, D3D11 and Metal backends

538 rows.

### src/d3d/d3d8_combiners.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/d3d/d3d8_combiners.c | - | g_ps_token | file_static | reset | hle_d3d8 | current guest pixel shader token |
| src/d3d/d3d8_combiners.c | - | g_combiner_state | file_static | reset | hle_d3d8 | parsed combiner state from guest render states |
| src/d3d/d3d8_combiners.c | - | g_dirty | file_static | reset | hle_d3d8 | set TRUE so next draw rebuilds combiner state |
| src/d3d/d3d8_combiners.c | - | g_combiner_cb | file_static | keep | - | host D3D11 PS constant buffer object |
| src/d3d/d3d8_combiners.c | - | g_cache | file_static | keep | - | compiled PS keyed by full combiner-state content |
| src/d3d/d3d8_combiners.c | - | g_frame_counter | file_static | keep | - | LRU clock for combiner cache, host only |

### src/d3d/d3d8_device.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/d3d/d3d8_device.c | - | g_device_state | file_static | reset | hle_d3d8 | render states/transforms/lights; keep device, swap chain, RTV fields |
| src/d3d/d3d8_device.c | - | g_device | file_static | keep | - | IDirect3DDevice8 singleton, vtable only |
| src/d3d/d3d8_device.c | - | g_device_initialized | once | keep | - | no rerun: device kept; CreateDevice must reuse, not recreate |
| src/d3d/d3d8_device.c | - | g_cur_vb | file_static | reset | hle_d3d8 | bound guest vertex buffer object |
| src/d3d/d3d8_device.c | - | g_cur_vb_stride | file_static | reset | hle_d3d8 | bound vertex buffer stride |
| src/d3d/d3d8_device.c | - | g_cur_ib | file_static | reset | hle_d3d8 | bound guest index buffer object |
| src/d3d/d3d8_device.c | - | g_cur_ib_base_vertex | file_static | reset | hle_d3d8 | bound index base vertex |
| src/d3d/d3d8_device.c | - | g_cur_textures | file_static | reset | hle_d3d8 | guest textures bound per stage |
| src/d3d/d3d8_device.c | - | g_cur_rt | file_static | reset | hle_d3d8 | guest render target surface bound |
| src/d3d/d3d8_device.c | - | g_cur_ds | file_static | reset | hle_d3d8 | guest depth-stencil surface bound |
| src/d3d/d3d8_device.c | - | g_palettes | file_static | reset | hle_d3d8 | guest-set palette entries per stage |
| src/d3d/d3d8_device.c | - | g_palettes_initialized | once | reset | hle_d3d8 | rerun: gray-ramp palette defaults rebuilt for new boot |
| src/d3d/d3d8_device.c | - | g_d3d_begin_count | file_static | keep | - | per-2s stats counter, log only |
| src/d3d/d3d8_device.c | - | g_d3d_end_count | file_static | keep | - | per-2s stats counter, log only |
| src/d3d/d3d8_device.c | - | g_d3d_clear_count | file_static | keep | - | per-2s stats counter, log only |
| src/d3d/d3d8_device.c | - | g_d3d_draw_count | file_static | keep | - | per-2s stats counter, log only |
| src/d3d/d3d8_device.c | - | g_d3d_settransform_count | file_static | keep | - | per-2s stats counter, log only |
| src/d3d/d3d8_device.c | - | g_d3d_setrs_count | file_static | keep | - | per-2s stats counter, log only |
| src/d3d/d3d8_device.c | - | g_d3d_settexture_count | file_static | keep | - | per-2s stats counter, log only |
| src/d3d/d3d8_device.c | dev_Present | frame_count | local_static | keep | - | FPS stats window counter, log only |
| src/d3d/d3d8_device.c | dev_Present | last_tick | local_static | keep | - | FPS stats window start tick |
| src/d3d/d3d8_device.c | - | g_up_ring_buffer | file_static | keep | - | host DrawPrimitiveUP ring buffer object |
| src/d3d/d3d8_device.c | - | g_up_ring_offset | file_static | keep | - | ring write offset, host transient |
| src/d3d/d3d8_device.c | - | g_d3d8 | file_static | keep | - | IDirect3D8 singleton, vtable only |
| src/d3d/d3d8_device.c | - | g_d3d8_ref | file_static | reset | hle_d3d8 | guest refcount on IDirect3D8; Direct3DCreate8 sets 1 |

### src/d3d/d3d8_gamma.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/d3d/d3d8_gamma.c | - | g_ramp | file_static | reset | hle_d3d8 | guest-set gamma ramp |
| src/d3d/d3d8_gamma.c | - | g_initialized | once | reset | hle_d3d8 | rerun: identity ramp default for new boot |
| src/d3d/d3d8_gamma.c | - | g_enabled | file_static | reset | hle_d3d8 | guest ramp is non-identity |
| src/d3d/d3d8_gamma.c | - | g_active | file_static | reset | hle_d3d8 | gamma pass active this present |
| src/d3d/d3d8_gamma.c | - | g_deferred | file_static | keep | - | host deferred context for gamma pass |
| src/d3d/d3d8_gamma.c | - | g_vs | file_static | keep | - | gamma VS compiled from fixed source |
| src/d3d/d3d8_gamma.c | - | g_ps | file_static | keep | - | gamma PS compiled from fixed source |
| src/d3d/d3d8_gamma.c | - | g_constants | file_static | keep | - | host gamma constant buffer object |
| src/d3d/d3d8_gamma.c | - | g_snapshot | file_static | keep | - | present-chain backbuffer snapshot texture |
| src/d3d/d3d8_gamma.c | - | g_source | file_static | keep | - | SRV of the backbuffer snapshot |

### src/d3d/d3d8_gl.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/d3d/d3d8_gl.c | - | g_window_title | file_static | keep | - | window title set by host config |
| src/d3d/d3d8_gl.c | - | g | file_static | reset | hle_d3d8 | D3D state/bindings reset; keep window, GL ctx, VAO, program |
| src/d3d/d3d8_gl.c | - | VS_SRC | file_static | constant | - | fixed GLSL vertex source pointer |
| src/d3d/d3d8_gl.c | - | FS_SRC | file_static | constant | - | fixed GLSL fragment source pointer |
| src/d3d/d3d8_gl.c | - | g_device | file_static | keep | - | IDirect3DDevice8 singleton, vtable only |
| src/d3d/d3d8_gl.c | - | g_d3d8 | file_static | keep | - | IDirect3D8 singleton, vtable only |
| src/d3d/d3d8_gl.c | - | g_suppress_present | global | reset | pgraph | set by pb replay while it owns frame |

### src/d3d/d3d8_resources.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/d3d/d3d8_resources.c | d3d8_check_overlay | checked | local_static | constant | - | one-time struct layout check, fixed |

### src/d3d/d3d8_shaders.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/d3d/d3d8_shaders.c | - | g_vs | file_static | keep | - | fixed-function VS from fixed source |
| src/d3d/d3d8_shaders.c | - | g_vs_blob | file_static | keep | - | bytecode of the fixed-function VS |
| src/d3d/d3d8_shaders.c | - | g_vs_cb | file_static | keep | - | host VS transform constant buffer |
| src/d3d/d3d8_shaders.c | - | g_vs_light_cb | file_static | keep | - | host VS lighting constant buffer |
| src/d3d/d3d8_shaders.c | - | g_ps_cb | file_static | keep | - | host PS constant buffer |
| src/d3d/d3d8_shaders.c | - | g_ff_ps_cache | file_static | keep | - | FF pixel shaders keyed by texture-dimension signature |
| src/d3d/d3d8_shaders.c | - | g_ff_ps_cache_count | file_static | keep | - | entries in FF PS cache |
| src/d3d/d3d8_shaders.c | - | g_layout_cache | file_static | keep | - | input layouts keyed by FVF |
| src/d3d/d3d8_shaders.c | - | g_layout_cache_count | file_static | keep | - | entries in layout cache |

### src/d3d/d3d8_states.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/d3d/d3d8_states.c | - | g_blend_state | file_static | keep | - | host blend state object, rebuilt on hash change |
| src/d3d/d3d8_states.c | - | g_ds_state | file_static | keep | - | host depth-stencil state object |
| src/d3d/d3d8_states.c | - | g_raster_state | file_static | keep | - | host rasterizer state object |
| src/d3d/d3d8_states.c | - | g_sampler_states | file_static | keep | - | host sampler state objects |
| src/d3d/d3d8_states.c | - | g_last_blend_hash | file_static | keep | - | content hash of g_blend_state, consistent with it |
| src/d3d/d3d8_states.c | - | g_last_raster_hash | file_static | keep | - | content hash of g_raster_state |
| src/d3d/d3d8_states.c | - | g_last_ds_desc | file_static | keep | - | desc of g_ds_state, consistent with it |

### src/d3d/d3d8_vsh.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/d3d/d3d8_vsh.c | - | g_vsh_slots | file_static | reset | hle_d3d8 | guest-created vertex shader handles/slots |
| src/d3d/d3d8_vsh.c | - | g_vsh_slot_count | file_static | reset | hle_d3d8 | count of guest vertex shader slots |
| src/d3d/d3d8_vsh.c | - | g_vsh_constants | file_static | reset | hle_d3d8 | guest VS constant registers c[192] |
| src/d3d/d3d8_vsh.c | - | g_vsh_constants_dirty | file_static | reset | hle_d3d8 | set TRUE so constants re-upload |
| src/d3d/d3d8_vsh.c | - | g_vsh_cb | file_static | keep | - | host VS constant buffer object |
| src/d3d/d3d8_vsh.c | - | g_vsh_cache | file_static | keep | - | compiled VS keyed by microcode hash |

### src/d3d/metal_compile.m

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/d3d/metal_compile.m | - | s_pinned | file_static | keep | - | MSL compile options, pinned version |
| src/d3d/metal_compile.m | - | s_os | file_static | keep | - | MSL compile options, OS default |
| src/d3d/metal_compile.m | - | s_version | file_static | constant | - | pinned MSL version from host |
| src/d3d/metal_compile.m | - | s_once | once | keep | - | no rerun: compile options built once, host only |
| src/d3d/metal_compile.m | metal_compile_library | said | local_static | keep | - | log-once guard for fallback compile |

### src/d3d/nv2a_pb_d3d11.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/d3d/nv2a_pb_d3d11.c | - | s_failed | file_static | keep | - | device init failed flag, host |
| src/d3d/nv2a_pb_d3d11.c | - | s_hwnd | file_static | keep | - | host window handle |
| src/d3d/nv2a_pb_d3d11.c | - | s_title_clock | file_static | keep | - | window title refresh clock, host |
| src/d3d/nv2a_pb_d3d11.c | - | s_sync_interval | file_static | keep | - | present vsync interval from env |
| src/d3d/nv2a_pb_d3d11.c | - | s_dev | file_static | keep | - | D3D11 device |
| src/d3d/nv2a_pb_d3d11.c | - | s_ctx | file_static | keep | - | D3D11 immediate context |
| src/d3d/nv2a_pb_d3d11.c | - | s_ctx1 | file_static | keep | - | D3D11.1 context for partial clears |
| src/d3d/nv2a_pb_d3d11.c | - | s_swap | file_static | keep | - | DXGI swap chain |
| src/d3d/nv2a_pb_d3d11.c | - | s_back | file_static | keep | - | swap chain back buffer |
| src/d3d/nv2a_pb_d3d11.c | - | s_back_w | file_static | keep | - | back buffer width |
| src/d3d/nv2a_pb_d3d11.c | - | s_back_h | file_static | keep | - | back buffer height |
| src/d3d/nv2a_pb_d3d11.c | - | s_made_w | file_static | keep | - | swap chain size at init |
| src/d3d/nv2a_pb_d3d11.c | - | s_made_h | file_static | keep | - | swap chain size at init |
| src/d3d/nv2a_pb_d3d11.c | - | s_client_w | file_static | keep | - | window client size from WM_SIZE |
| src/d3d/nv2a_pb_d3d11.c | - | s_client_h | file_static | keep | - | window client size from WM_SIZE |
| src/d3d/nv2a_pb_d3d11.c | - | s_back_rtv | file_static | keep | - | back buffer RTV for the scaling blit |
| src/d3d/nv2a_pb_d3d11.c | - | s_dump_rt | file_static | keep | - | frame dump source choice, host option |
| src/d3d/nv2a_pb_d3d11.c | - | s_compile | file_static | constant | - | D3DCompile function pointer from loaded DLL |
| src/d3d/nv2a_pb_d3d11.c | - | s_create_blob | file_static | constant | - | D3DCreateBlob function pointer from loaded DLL |
| src/d3d/nv2a_pb_d3d11.c | - | s_shc | file_static | keep | - | on-disk DXBC cache keyed by source and compiler |
| src/d3d/nv2a_pb_d3d11.c | - | s_shc_compiler | file_static | constant | - | compiler identity string for cache keys |
| src/d3d/nv2a_pb_d3d11.c | - | s_sh_compiled | file_static | keep | - | compile count stats |
| src/d3d/nv2a_pb_d3d11.c | - | s_sh_cached | file_static | keep | - | cache-hit count stats |
| src/d3d/nv2a_pb_d3d11.c | - | s_sh_compile_ms | file_static | keep | - | compile time stats |
| src/d3d/nv2a_pb_d3d11.c | - | s_sh_cached_ms | file_static | keep | - | cache-load time stats |
| src/d3d/nv2a_pb_d3d11.c | - | s_cb_consts | file_static | keep | - | host constant buffer object c[192] |
| src/d3d/nv2a_pb_d3d11.c | - | s_cb_vp | file_static | keep | - | host viewport/fog constant buffer object |
| src/d3d/nv2a_pb_d3d11.c | - | s_cb_ps | file_static | keep | - | host PS constant buffer object |
| src/d3d/nv2a_pb_d3d11.c | - | s_memo | file_static | keep | - | cached RENV_D3D11_MEMO read |
| src/d3d/nv2a_pb_d3d11.c | - | s_state_gen | file_static | keep | - | monotonic bound-state generation; bump, never zero |
| src/d3d/nv2a_pb_d3d11.c | - | s_sc | file_static | reset | renderer.d3d11 | bound-state filter; may name released RT views |
| src/d3d/nv2a_pb_d3d11.c | - | s_sc_init | once | reset | renderer.d3d11 | rerun: clear s_sc after caches drop and ClearState |
| src/d3d/nv2a_pb_d3d11.c | - | s_vb | file_static | keep | - | host vertex ring buffer object |
| src/d3d/nv2a_pb_d3d11.c | - | s_vb_size | file_static | keep | - | vertex ring size |
| src/d3d/nv2a_pb_d3d11.c | - | s_vb_pos | file_static | keep | - | vertex ring position, host transient |
| src/d3d/nv2a_pb_d3d11.c | - | s_ib | file_static | keep | - | host index ring buffer object |
| src/d3d/nv2a_pb_d3d11.c | - | s_ib_size | file_static | keep | - | index ring size |
| src/d3d/nv2a_pb_d3d11.c | - | s_ib_pos | file_static | keep | - | index ring position, host transient |
| src/d3d/nv2a_pb_d3d11.c | - | s_rs | file_static | keep | - | rasterizer states keyed by cull/winding |
| src/d3d/nv2a_pb_d3d11.c | - | s_rs_sc | file_static | keep | - | scissored rasterizer states |
| src/d3d/nv2a_pb_d3d11.c | - | s_ds_off | file_static | keep | - | depth-off state object |
| src/d3d/nv2a_pb_d3d11.c | - | s_ps | file_static | keep | - | basic PS from fixed source |
| src/d3d/nv2a_pb_d3d11.c | - | s_presents | file_static | keep | - | present count stats and debug dump trigger |
| src/d3d/nv2a_pb_d3d11.c | - | s_clears | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_draws | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_draws_skipped | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_draws_culled | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_draws_bconst | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_draws_stretch | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_tex_uploads | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_tex_rehash | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_tex_changed | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_tex_mip_uploads | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_vb_native | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_vb_expanded | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_draws_prog | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_draws_tex | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_rt | file_static | reset | renderer.d3d11 | render targets keyed by guest surface offset/address |
| src/d3d/nv2a_pb_d3d11.c | - | s_drawn | file_static | reset | renderer.d3d11 | points into s_rt: last presented target |
| src/d3d/nv2a_pb_d3d11.c | - | s_cur | file_static | reset | renderer.d3d11 | points into s_rt: target being drawn |
| src/d3d/nv2a_pb_d3d11.c | - | s_tick | file_static | keep | - | monotonic LRU clock |
| src/d3d/nv2a_pb_d3d11.c | - | s_seq | file_static | keep | - | monotonic draw/clear order |
| src/d3d/nv2a_pb_d3d11.c | - | s_rtt_binds | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_loop_hits | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_vs | file_static | keep | - | VS/layouts keyed by vertex program content hash |
| src/d3d/nv2a_pb_d3d11.c | - | s_blend | file_static | keep | - | blend states keyed by state words |
| src/d3d/nv2a_pb_d3d11.c | - | s_blend_n | file_static | keep | - | blend cache fill count |
| src/d3d/nv2a_pb_d3d11.c | pump | quit_at | local_static | keep | - | bench quit deadline from env, host |
| src/d3d/nv2a_pb_d3d11.c | pump | quit_posted | local_static | keep | - | bench quit already posted |
| src/d3d/nv2a_pb_d3d11.c | sh_now_ms | f | local_static | constant | - | QPC frequency |
| src/d3d/nv2a_pb_d3d11.c | init | tried | local_static | keep | - | device init ran once; device kept |
| src/d3d/nv2a_pb_d3d11.c | - | s_rt_gen | file_static | keep | - | monotonic RT-set gen; rt_release bumps it |
| src/d3d/nv2a_pb_d3d11.c | - | s_stale_drops | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_retired | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_evict_lost | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_grows | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_blits_gpu | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_blits_guest | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_blit_writebacks | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_blit_dropped | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | blit_write_back | row | local_static | keep | - | row scratch buffer, refilled per write-back |
| src/d3d/nv2a_pb_d3d11.c | blit_write_back | row_cap | local_static | keep | - | row scratch buffer, refilled per write-back |
| src/d3d/nv2a_pb_d3d11.c | aspect_copy_tracks | said | local_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_d3d11.c | d3d11_on_blit | said | local_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_d3d11.c | - | s_partial_clears | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | alias_check_on | on | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_d3d11.c | rt_drop_if_stale | said | local_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_d3d11.c | rt_grow | said | local_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_d3d11.c | - | s_dt | file_static | reset | renderer.d3d11 | depth targets keyed by guest zeta offset |
| src/d3d/nv2a_pb_d3d11.c | - | s_dss | file_static | keep | - | depth-stencil states keyed by state key |
| src/d3d/nv2a_pb_d3d11.c | - | s_dss_count | file_static | keep | - | DSS cache fill count |
| src/d3d/nv2a_pb_d3d11.c | - | s_zclears | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | - | s_draws_z | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | clear_region | said | local_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_d3d11.c | - | s_vbm_base | file_static | keep | - | per-batch ring mapping, idle between batches |
| src/d3d/nv2a_pb_d3d11.c | - | s_vbm_at | file_static | keep | - | per-batch ring mapping, idle between batches |
| src/d3d/nv2a_pb_d3d11.c | - | s_vbm_used | file_static | keep | - | per-batch ring mapping, idle between batches |
| src/d3d/nv2a_pb_d3d11.c | - | s_vbm_cap | file_static | keep | - | per-batch ring mapping, idle between batches |
| src/d3d/nv2a_pb_d3d11.c | vertex_shader | parsed | local_static | keep | - | scratch parse buffer |
| src/d3d/nv2a_pb_d3d11.c | vertex_shader | hlsl | local_static | keep | - | scratch HLSL buffer |
| src/d3d/nv2a_pb_d3d11.c | vertex_shader | memo | local_static | reset | renderer.d3d11 | keyed on prog_dwords/start; pgraph counter rewinds at reset |
| src/d3d/nv2a_pb_d3d11.c | rasterizer_state | off | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_d3d11.c | rasterizer_state | flip | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_d3d11.c | rs_biased | e | local_static | keep | - | depth-bias rasterizer states keyed by their whole description |
| src/d3d/nv2a_pb_d3d11.c | rs_biased | n | local_static | keep | - | depth-bias rasterizer cache fill |
| src/d3d/nv2a_pb_d3d11.c | rs_biased | logged | local_static | keep | - | once-per-process warning that the bias cache is full |
| src/d3d/nv2a_pb_d3d11.c | - | s_ps_w | file_static | keep | - | set by setup_pixel before every batch's binds |
| src/d3d/nv2a_pb_d3d11.c | st_w_on | cb | local_static | keep | - | W-depth constant buffer object |
| src/d3d/nv2a_pb_d3d11.c | st_w_on | shadow | local_static | keep | - | last W-depth constants uploaded; only skips a same-value Map |
| src/d3d/nv2a_pb_d3d11.c | st_w_on | ok | local_static | keep | - | buffer creation once-guard, host |
| src/d3d/nv2a_pb_d3d11.c | blend_state | next | local_static | keep | - | round-robin eviction index |
| src/d3d/nv2a_pb_d3d11.c | blend_color | warned | local_static | keep | - | warn-once guard |
| src/d3d/nv2a_pb_d3d11.c | - | s_stage | file_static | reset | renderer.d3d11 | stages decoded from guest texture registers |
| src/d3d/nv2a_pb_d3d11.c | - | s_stage_src | file_static | reset | renderer.d3d11 | per-stage SRV source, trace |
| src/d3d/nv2a_pb_d3d11.c | - | s_stage_rt | file_static | reset | renderer.d3d11 | per-stage pointers into s_rt |
| src/d3d/nv2a_pb_d3d11.c | - | s_tc | file_static | reset | renderer.d3d11 | textures keyed by guest offset/format, from guest memory |
| src/d3d/nv2a_pb_d3d11.c | tex_cache_n | n | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_d3d11.c | - | s_tc_clock | file_static | keep | - | monotonic texture LRU clock |
| src/d3d/nv2a_pb_d3d11.c | - | s_tc_hint | file_static | reset | renderer.d3d11 | key hash to s_tc slot hints |
| src/d3d/nv2a_pb_d3d11.c | - | s_texels | file_static | keep | - | host decode scratch buffer |
| src/d3d/nv2a_pb_d3d11.c | - | s_texels_cap | file_static | keep | - | scratch buffer capacity |
| src/d3d/nv2a_pb_d3d11.c | - | s_black_srv | file_static | keep | - | fixed black texture view |
| src/d3d/nv2a_pb_d3d11.c | - | s_magenta_srv | file_static | keep | - | fixed magenta texture view |
| src/d3d/nv2a_pb_d3d11.c | - | s_black_cube_srv | file_static | keep | - | fixed black cube texture view |
| src/d3d/nv2a_pb_d3d11.c | no_mips | off | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_d3d11.c | - | s_stage_gen | file_static | keep | - | monotonic stage-decode gen; bump, never zero |
| src/d3d/nv2a_pb_d3d11.c | build_stages | memo | local_static | reset | renderer.d3d11 | keyed on tex-regs gen/contig; both rewind at reset |
| src/d3d/nv2a_pb_d3d11.c | build_stages | said | local_static | keep | - | log-once guard table |
| src/d3d/nv2a_pb_d3d11.c | - | s_tc_gen | file_static | keep | - | monotonic tex-cache gen; drop path bumps it |
| src/d3d/nv2a_pb_d3d11.c | - | s_tex_last | file_static | reset | renderer.d3d11 | points into s_tc |
| src/d3d/nv2a_pb_d3d11.c | tex_bind_body | said | local_static | keep | - | log-once guard table |
| src/d3d/nv2a_pb_d3d11.c | - | s_smp | file_static | keep | - | sampler states keyed by sampler state |
| src/d3d/nv2a_pb_d3d11.c | - | s_smp_n | file_static | keep | - | sampler cache fill count |
| src/d3d/nv2a_pb_d3d11.c | - | s_smp_last | file_static | keep | - | per-stage index into kept sampler cache, key-checked |
| src/d3d/nv2a_pb_d3d11.c | sampler | point | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_d3d11.c | sampler | fallback | local_static | keep | - | host fallback sampler object |
| src/d3d/nv2a_pb_d3d11.c | sampler | next | local_static | keep | - | round-robin eviction index |
| src/d3d/nv2a_pb_d3d11.c | - | s_rc | file_static | keep | - | combiner PS keyed by combiner-state content |
| src/d3d/nv2a_pb_d3d11.c | - | s_rc_compiles | file_static | keep | - | compile counter, memo check for kept cache |
| src/d3d/nv2a_pb_d3d11.c | - | s_rc_last | file_static | keep | - | points into kept s_rc |
| src/d3d/nv2a_pb_d3d11.c | snap_ps | tried | local_static | keep | - | snap PS built once from fixed source |
| src/d3d/nv2a_pb_d3d11.c | snap_ps | ps | local_static | keep | - | snap PS from fixed source |
| src/d3d/nv2a_pb_d3d11.c | combiner_ps | src | local_static | keep | - | scratch MSL/HLSL source buffer |
| src/d3d/nv2a_pb_d3d11.c | - | s_self | file_static | reset | renderer.d3d11 | self-sample copy of a guest frame target |
| src/d3d/nv2a_pb_d3d11.c | - | s_self_copies | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_d3d11.c | rt_self_copy | said | local_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_d3d11.c | rt_texture | off | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_d3d11.c | - | s_last_rc | file_static | keep | - | paired with setup_pixel memo; reset both or neither |
| src/d3d/nv2a_pb_d3d11.c | - | s_last_rc_on | file_static | keep | - | per-batch flag, rewritten before use |
| src/d3d/nv2a_pb_d3d11.c | - | s_dbg_post | file_static | keep | - | per-batch debug flag, rewritten before use |
| src/d3d/nv2a_pb_d3d11.c | debug_ps | tried | local_static | keep | - | debug PS built once from env |
| src/d3d/nv2a_pb_d3d11.c | debug_ps | ps | local_static | keep | - | debug PS from env |
| src/d3d/nv2a_pb_d3d11.c | - | s_cb_ps_shadow | file_static | keep | - | bytes held by kept PS buffer, memcmp-checked |
| src/d3d/nv2a_pb_d3d11.c | - | s_cb_ps_ok | file_static | keep | - | shadow valid; buffer and shadow both kept |
| src/d3d/nv2a_pb_d3d11.c | setup_pixel | bump_trace | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_d3d11.c | setup_pixel | lm | local_static | reset | renderer.d3d11 | stage-loop memo of s_tc views, keyed on flips |
| src/d3d/nv2a_pb_d3d11.c | setup_pixel | memo | local_static | keep | - | keyed by combiner register content, kept s_rc |
| src/d3d/nv2a_pb_d3d11.c | setup_pixel | st0 | local_static | keep | - | scratch copy of s_last_rc for the W fallback, rewritten before each use |
| src/d3d/nv2a_pb_d3d11.c | read_tex_pixel | st | local_static | keep | - | debug staging texture |
| src/d3d/nv2a_pb_d3d11.c | trace_stream_out | buf | local_static | keep | - | debug stream-out buffer |
| src/d3d/nv2a_pb_d3d11.c | trace_stream_out | stage | local_static | keep | - | debug staging buffer |
| src/d3d/nv2a_pb_d3d11.c | trace_print_hlsl_once | seen | local_static | keep | - | log-once key table |
| src/d3d/nv2a_pb_d3d11.c | trace_print_hlsl_once | n | local_static | keep | - | log-once key count |
| src/d3d/nv2a_pb_d3d11.c | trace_batch | all | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_d3d11.c | trace_batch | parsed | local_static | keep | - | trace scratch parse buffer |
| src/d3d/nv2a_pb_d3d11.c | trace_batch | hlsl | local_static | keep | - | trace scratch HLSL buffer |
| src/d3d/nv2a_pb_d3d11.c | - | s_px_n | file_static | keep | - | debug probe pixel count from env |
| src/d3d/nv2a_pb_d3d11.c | - | s_px_x | file_static | keep | - | debug probe x from env |
| src/d3d/nv2a_pb_d3d11.c | - | s_px_y | file_static | keep | - | debug probe y from env |
| src/d3d/nv2a_pb_d3d11.c | - | s_px_max | file_static | keep | - | debug probe batch cap |
| src/d3d/nv2a_pb_d3d11.c | - | s_px_f0 | file_static | keep | - | debug probe flip window |
| src/d3d/nv2a_pb_d3d11.c | - | s_px_f1 | file_static | keep | - | debug probe flip window |
| src/d3d/nv2a_pb_d3d11.c | px_shader | ps | local_static | keep | - | debug probe PSs from fixed source |
| src/d3d/nv2a_pb_d3d11.c | px_shader | tried | local_static | keep | - | debug probe PS build guard |
| src/d3d/nv2a_pb_d3d11.c | px_probe | tex | local_static | keep | - | debug probe textures |
| src/d3d/nv2a_pb_d3d11.c | px_probe | st | local_static | keep | - | debug probe staging texture |
| src/d3d/nv2a_pb_d3d11.c | px_probe | rtv | local_static | keep | - | debug probe RTVs |
| src/d3d/nv2a_pb_d3d11.c | px_probe | tw | local_static | keep | - | debug probe texture width |
| src/d3d/nv2a_pb_d3d11.c | px_probe | th | local_static | keep | - | debug probe texture height |
| src/d3d/nv2a_pb_d3d11.c | px_probe | dss_le | local_static | keep | - | debug probe depth state |
| src/d3d/nv2a_pb_d3d11.c | px_probe | names | local_static | keep | - | debug probe name table |
| src/d3d/nv2a_pb_d3d11.c | aspect_trace | on | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_d3d11.c | aspect_log | set | local_static | keep | - | log-once key set |
| src/d3d/nv2a_pb_d3d11.c | aspect_log | used | local_static | keep | - | log-once key count |
| src/d3d/nv2a_pb_d3d11.c | aspect_log_post | set | local_static | keep | - | log-once key set |
| src/d3d/nv2a_pb_d3d11.c | aspect_log_post | used | local_static | keep | - | log-once key count |
| src/d3d/nv2a_pb_d3d11.c | aspect_log_flat | set | local_static | keep | - | log-once key set |
| src/d3d/nv2a_pb_d3d11.c | aspect_log_flat | used | local_static | keep | - | log-once key count |
| src/d3d/nv2a_pb_d3d11.c | aspect_flat | vf | local_static | keep | - | scratch vertex sample buffer |
| src/d3d/nv2a_pb_d3d11.c | set_vp | vp_shadow | local_static | keep | - | bytes held by kept viewport buffer, memcmp-checked |
| src/d3d/nv2a_pb_d3d11.c | set_vp | vp_ok | local_static | keep | - | shadow valid; buffer kept |
| src/d3d/nv2a_pb_d3d11.c | d3d11_on_draw | idx | local_static | keep | - | scratch index buffer |
| src/d3d/nv2a_pb_d3d11.c | d3d11_on_draw | idx_cap | local_static | keep | - | scratch capacity |
| src/d3d/nv2a_pb_d3d11.c | d3d11_on_draw | hxy | local_static | keep | - | scratch hor+ vertex xy |
| src/d3d/nv2a_pb_d3d11.c | d3d11_on_draw | hxy_cap | local_static | keep | - | scratch capacity |
| src/d3d/nv2a_pb_d3d11.c | d3d11_on_draw | hvf | local_static | keep | - | scratch hor+ inputs |
| src/d3d/nv2a_pb_d3d11.c | d3d11_on_draw | hvf_cap | local_static | keep | - | scratch capacity |
| src/d3d/nv2a_pb_d3d11.c | d3d11_on_draw | consts_shadow | local_static | keep | - | bytes held by kept c[] buffer |
| src/d3d/nv2a_pb_d3d11.c | d3d11_on_draw | consts_ok | local_static | reset | renderer.d3d11 | gen memo valid; pgraph consts gen rewinds |
| src/d3d/nv2a_pb_d3d11.c | d3d11_on_draw | consts_gen | local_static | reset | renderer.d3d11 | pgraph consts gen last uploaded; rewinds at reset |
| src/d3d/nv2a_pb_d3d11.c | d3d11_on_draw | skip_post | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_d3d11.c | - | s_shown_w | file_static | keep | - | last present extent, host display |
| src/d3d/nv2a_pb_d3d11.c | - | s_shown_h | file_static | keep | - | last present extent, host display |
| src/d3d/nv2a_pb_d3d11.c | - | s_chained | file_static | keep | - | this flip used present.shader, per flip |
| src/d3d/nv2a_pb_d3d11.c | dump_frame | prefix | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_d3d11.c | dump_frame | checked | local_static | keep | - | env read guard |
| src/d3d/nv2a_pb_d3d11.c | dump_frame | warned | local_static | keep | - | warn-once guard |
| src/d3d/nv2a_pb_d3d11.c | - | s_occ | file_static | reset | renderer.d3d11 | pending occlusion reports at guest VAs; return queries |
| src/d3d/nv2a_pb_d3d11.c | flip_log | on | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_draws | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_prog | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_tex | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_rtt | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_clears | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_up | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_chg | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_re | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_vn | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_ve | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_skip | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_cull | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_self | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_mip | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_occ | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_occ_vis | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_occ_us | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_occ_late | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_occ_super | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_occ_set | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_occ_stale | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_occ_first | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_loop | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | flip_log | zt | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_d3d11.c | flip_log | p_stretch | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_d3d11.c | - | s_blit_vs | file_static | keep | - | present blit VS |
| src/d3d/nv2a_pb_d3d11.c | - | s_blit_ps | file_static | keep | - | present blit PS |
| src/d3d/nv2a_pb_d3d11.c | - | s_blit_smp | file_static | keep | - | present blit samplers |
| src/d3d/nv2a_pb_d3d11.c | - | s_blit_cb | file_static | keep | - | present blit constant buffer |
| src/d3d/nv2a_pb_d3d11.c | - | s_blit_failed | file_static | keep | - | present blit build failed flag |
| src/d3d/nv2a_pb_d3d11.c | - | s_ovl_place | file_static | keep | - | where this present placed the frame |
| src/d3d/nv2a_pb_d3d11.c | - | s_lrs_chain | file_static | keep | - | librashader present chain |
| src/d3d/nv2a_pb_d3d11.c | - | s_lrs_gen | file_static | keep | - | present chain request generation applied |
| src/d3d/nv2a_pb_d3d11.c | - | s_lrs_frames | file_static | keep | - | frames fed to the present chain |
| src/d3d/nv2a_pb_d3d11.c | - | s_lrs_lines | file_static | keep | - | present chain preset lines per source line |
| src/d3d/nv2a_pb_d3d11.c | - | s_lrs_said | file_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_d3d11.c | - | s_lrs_src | file_static | keep | - | present chain input copy texture |
| src/d3d/nv2a_pb_d3d11.c | - | s_lrs_srv | file_static | keep | - | present chain input view |
| src/d3d/nv2a_pb_d3d11.c | - | s_lrs_src_rtv | file_static | keep | - | present chain resolve RTV |
| src/d3d/nv2a_pb_d3d11.c | - | s_lrs_out | file_static | keep | - | present chain output texture |
| src/d3d/nv2a_pb_d3d11.c | - | s_lrs_out_rtv | file_static | keep | - | present chain output RTV |
| src/d3d/nv2a_pb_d3d11.c | - | s_lrs_res_ps | file_static | keep | - | present chain resolve PS |
| src/d3d/nv2a_pb_d3d11.c | - | s_lrs_res_cb | file_static | keep | - | present chain resolve constant buffer |
| src/d3d/nv2a_pb_d3d11.c | - | s_lrs_res_failed | file_static | keep | - | resolve build failed flag |
| src/d3d/nv2a_pb_d3d11.c | - | s_lrs_ff | file_static | constant | - | librashader frame function pointer |
| src/d3d/nv2a_pb_d3d11.c | - | s_lrs_sp | file_static | constant | - | librashader set_param function pointer |
| src/d3d/nv2a_pb_d3d11.c | - | s_lrs_pgen | file_static | keep | - | present chain parameter generation applied |
| src/d3d/nv2a_pb_d3d11.c | - | s_lrs_ov_said | file_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_d3d11.c | chain_surface | s_popup | local_static | keep | - | window popup style state |
| src/d3d/nv2a_pb_d3d11.c | - | s_ovl_vs | file_static | keep | - | overlay VS |
| src/d3d/nv2a_pb_d3d11.c | - | s_ovl_ps | file_static | keep | - | overlay PS |
| src/d3d/nv2a_pb_d3d11.c | - | s_ovl_il | file_static | keep | - | overlay input layout |
| src/d3d/nv2a_pb_d3d11.c | - | s_ovl_bs | file_static | keep | - | overlay blend state |
| src/d3d/nv2a_pb_d3d11.c | - | s_ovl_cb | file_static | keep | - | overlay constant buffer |
| src/d3d/nv2a_pb_d3d11.c | - | s_ovl_vb | file_static | keep | - | overlay vertex buffer |
| src/d3d/nv2a_pb_d3d11.c | - | s_ovl_ib | file_static | keep | - | overlay index buffer |
| src/d3d/nv2a_pb_d3d11.c | - | s_ovl_vb_cap | file_static | keep | - | overlay buffer capacity |
| src/d3d/nv2a_pb_d3d11.c | - | s_ovl_ib_cap | file_static | keep | - | overlay buffer capacity |
| src/d3d/nv2a_pb_d3d11.c | - | s_ovl_srv | file_static | keep | - | overlay/menu image views |
| src/d3d/nv2a_pb_d3d11.c | - | s_ovl_srv_gen | file_static | keep | - | overlay image generations |
| src/d3d/nv2a_pb_d3d11.c | - | s_ovl_failed | file_static | keep | - | overlay build failed flag |
| src/d3d/nv2a_pb_d3d11.c | - | s_gamescope | file_static | keep | - | cached RENV_HOST_GAMESCOPE read |
| src/d3d/nv2a_pb_d3d11.c | - | s_dbg_vs | file_static | keep | - | debug-draw VS |
| src/d3d/nv2a_pb_d3d11.c | - | s_dbg_ps | file_static | keep | - | debug-draw PS |
| src/d3d/nv2a_pb_d3d11.c | - | s_dbg_il | file_static | keep | - | debug-draw input layout |
| src/d3d/nv2a_pb_d3d11.c | - | s_dbg_bs | file_static | keep | - | debug-draw blend state |
| src/d3d/nv2a_pb_d3d11.c | - | s_dbg_ds | file_static | keep | - | debug-draw depth states |
| src/d3d/nv2a_pb_d3d11.c | - | s_dbg_vb | file_static | keep | - | debug-draw vertex buffer |
| src/d3d/nv2a_pb_d3d11.c | - | s_dbg_cb | file_static | keep | - | debug-draw constant buffer |
| src/d3d/nv2a_pb_d3d11.c | - | s_dbg_vb_cap | file_static | keep | - | debug-draw buffer capacity |
| src/d3d/nv2a_pb_d3d11.c | - | s_dbg_failed | file_static | keep | - | debug-draw build failed flag |
| src/d3d/nv2a_pb_d3d11.c | - | s_probe_x | file_static | keep | - | back-pixel probe x, host |
| src/d3d/nv2a_pb_d3d11.c | - | s_probe_y | file_static | keep | - | back-pixel probe y, host |
| src/d3d/nv2a_pb_d3d11.c | - | s_probe_px | file_static | keep | - | last probed back-buffer pixel |
| src/d3d/nv2a_pb_d3d11.c | gpu_mem_note | a3 | local_static | keep | - | DXGI adapter for memory stats |
| src/d3d/nv2a_pb_d3d11.c | gpu_mem_note | a3_dev | local_static | keep | - | device the adapter belongs to |
| src/d3d/nv2a_pb_d3d11.c | gpu_mem_note | said | local_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_d3d11.c | d3d11_flip | said | local_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_d3d11.c | d3d11_flip | said_nb | local_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_d3d11.c | fullscreen_switch | s_windowed | local_static | keep | - | saved windowed placement |
| src/d3d/nv2a_pb_d3d11.c | fullscreen_switch | s_have_windowed | local_static | keep | - | saved placement valid |
| src/d3d/nv2a_pb_d3d11.c | - | s_occ_pool | file_static | keep | - | free host query objects |
| src/d3d/nv2a_pb_d3d11.c | - | s_occ_pool_n | file_static | keep | - | free query count |
| src/d3d/nv2a_pb_d3d11.c | - | s_occ_made | file_static | keep | - | queries made total |
| src/d3d/nv2a_pb_d3d11.c | - | s_occ_mode | file_static | keep | - | cached RENV_D3D11_OCC read |
| src/d3d/nv2a_pb_d3d11.c | occ_now_us | f | local_static | constant | - | QPC frequency |
| src/d3d/nv2a_pb_d3d11.c | - | s_mag_buf | file_static | keep | - | the tex_mag trace, a debug figure keyed by content hash |
| src/d3d/nv2a_pb_d3d11.c | - | s_mag_read | file_static | keep | - | the tex_mag trace, a debug figure keyed by content hash |
| src/d3d/nv2a_pb_d3d11.c | - | s_mag_cb | file_static | keep | - | the tex_mag trace, a debug figure keyed by content hash |
| src/d3d/nv2a_pb_d3d11.c | - | s_mag_uav | file_static | keep | - | the tex_mag trace, a debug figure keyed by content hash |
| src/d3d/nv2a_pb_d3d11.c | - | s_mag_rec | file_static | keep | - | the tex_mag trace, a debug figure keyed by content hash |
| src/d3d/nv2a_pb_d3d11.c | - | s_mag_n | file_static | keep | - | the tex_mag trace, a debug figure keyed by content hash |
| src/d3d/nv2a_pb_d3d11.c | - | s_mag_cur | file_static | keep | - | the tex_mag trace, a debug figure keyed by content hash |
| src/d3d/nv2a_pb_d3d11.c | - | s_mag_copied | file_static | keep | - | the tex_mag trace, a debug figure keyed by content hash |
| src/d3d/nv2a_pb_d3d11.c | - | s_mag_state | file_static | keep | - | the tex_mag trace, a debug figure keyed by content hash |
| src/d3d/nv2a_pb_d3d11.c | - | s_mag_hash | file_static | keep | - | the tex_mag trace and the dump's facts, debug figures keyed by content hash |
| src/d3d/nv2a_pb_d3d11.c | - | s_mag_scale | file_static | keep | - | the tex_mag trace, a debug figure keyed by content hash |

### src/d3d/nv2a_pb_metal.m

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/d3d/nv2a_pb_metal.m | - | s_layer | file_static | keep | - | window takes GPU slot textures, host mode |
| src/d3d/nv2a_pb_metal.m | - | s_window_presents | file_static | keep | - | frames sent to window, stats |
| src/d3d/nv2a_pb_metal.m | - | s_dev | file_static | keep | - | Metal device adopted from the window |
| src/d3d/nv2a_pb_metal.m | - | s_queue | file_static | keep | - | Metal command queue adopted from the window |
| src/d3d/nv2a_pb_metal.m | - | s_vs_pass | file_static | keep | - | pass-through VS from fixed source |
| src/d3d/nv2a_pb_metal.m | - | s_fs_basic | file_static | keep | - | basic FS from fixed source |
| src/d3d/nv2a_pb_metal.m | - | s_vs_clear | file_static | keep | - | partial-clear VS |
| src/d3d/nv2a_pb_metal.m | - | s_fs_clear | file_static | keep | - | partial-clear FS |
| src/d3d/nv2a_pb_metal.m | - | s_clear_pso | file_static | keep | - | partial-clear pipelines |
| src/d3d/nv2a_pb_metal.m | - | s_cmd | file_static | reset | renderer.metal | open frame command buffer; drain commits, waits, nils |
| src/d3d/nv2a_pb_metal.m | - | s_enc | file_static | reset | renderer.metal | open render encoder; end_encoder before drops |
| src/d3d/nv2a_pb_metal.m | - | s_failed | file_static | keep | - | init failed flag, host |
| src/d3d/nv2a_pb_metal.m | - | s_unified | file_static | constant | - | device has unified memory |
| src/d3d/nv2a_pb_metal.m | - | s_bc | file_static | constant | - | device samples BC3 |
| src/d3d/nv2a_pb_metal.m | - | s_vb | file_static | keep | - | host vertex ring buffer |
| src/d3d/nv2a_pb_metal.m | - | s_ib | file_static | keep | - | host index ring buffer |
| src/d3d/nv2a_pb_metal.m | - | s_vb_size | file_static | keep | - | vertex ring size |
| src/d3d/nv2a_pb_metal.m | - | s_vb_pos | file_static | keep | - | vertex ring position, rewound after each wait |
| src/d3d/nv2a_pb_metal.m | - | s_ib_size | file_static | keep | - | index ring size |
| src/d3d/nv2a_pb_metal.m | - | s_ib_pos | file_static | keep | - | index ring position, rewound after each wait |
| src/d3d/nv2a_pb_metal.m | - | s_presents | file_static | keep | - | present count stats and debug dump trigger |
| src/d3d/nv2a_pb_metal.m | - | s_clears | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_draws | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_draws_skipped | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_draws_prog | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_draws_culled | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_draws_tex | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_tex_uploads | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_tex_rehash | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_tex_changed | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_flushes | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_writebacks | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_cmd_errors | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_draws_stretch | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_rt | file_static | reset | renderer.metal | render targets keyed by guest surface; drop without write-back |
| src/d3d/nv2a_pb_metal.m | - | s_enc_rt | file_static | reset | renderer.metal | points into s_rt: encoder target |
| src/d3d/nv2a_pb_metal.m | - | s_enc_dt | file_static | reset | renderer.metal | points into s_dt: encoder depth |
| src/d3d/nv2a_pb_metal.m | - | s_dt | file_static | reset | renderer.metal | depth targets keyed by guest zeta offset |
| src/d3d/nv2a_pb_metal.m | - | s_tick | file_static | keep | - | monotonic LRU clock |
| src/d3d/nv2a_pb_metal.m | - | s_zclears | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_draws_z | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_rtt_binds | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_self_copies | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_stale_drops | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_retired | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_evict_lost | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_grows | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_blits_gpu | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_blits_guest | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | aspect_copy_tracks | said | local_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_metal.m | metal_on_blit | said | local_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_metal.m | - | s_partial_clears | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_vis | file_static | keep | - | host visibility result buffer |
| src/d3d/nv2a_pb_metal.m | - | s_occ_q | file_static | reset | renderer.metal | query counting on the open pass |
| src/d3d/nv2a_pb_metal.m | - | s_occ_slot | file_static | reset | renderer.metal | visibility slot on the open encoder |
| src/d3d/nv2a_pb_metal.m | - | s_prof | file_static | keep | - | cached RENV_METAL_PROF read |
| src/d3d/nv2a_pb_metal.m | - | s_prof_cmd | file_static | keep | - | profiled command buffers, released at flip |
| src/d3d/nv2a_pb_metal.m | - | s_prof_ncmd | file_static | keep | - | profiled command buffer count |
| src/d3d/nv2a_pb_metal.m | - | s_pa | file_static | keep | - | profiler totals |
| src/d3d/nv2a_pb_metal.m | - | s_last_cmd | file_static | keep | - | last committed command buffer, waited by drain |
| src/d3d/nv2a_pb_metal.m | - | s_decode_syncs | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_pitch_alias_binds | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_pitch_alias_seen | file_static | keep | - | trace log-once pair set |
| src/d3d/nv2a_pb_metal.m | - | s_sh_trace | file_static | keep | - | cached RENV_METAL_SHADERS read |
| src/d3d/nv2a_pb_metal.m | - | s_sh_flip | file_static | keep | - | per-flip shader build timing stats |
| src/d3d/nv2a_pb_metal.m | - | s_enc_serial | file_static | keep | - | monotonic encoder serial; never zero |
| src/d3d/nv2a_pb_metal.m | - | s_vc1 | file_static | reset | renderer.metal | c[] gen memo on encoder; pgraph gen rewinds |
| src/d3d/nv2a_pb_metal.m | - | s_c_uploads | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | init | tried | local_static | keep | - | device init ran once; device kept |
| src/d3d/nv2a_pb_metal.m | alias_check_on | on | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_metal.m | rt_drop_if_stale | said | local_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_metal.m | rt_grow | said | local_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_metal.m | clear_pso | failed | local_static | keep | - | pipeline build failed flags |
| src/d3d/nv2a_pb_metal.m | - | s_pso | file_static | keep | - | pipelines keyed by kept functions and blend key |
| src/d3d/nv2a_pb_metal.m | - | s_pso_n | file_static | keep | - | pipeline cache fill count |
| src/d3d/nv2a_pb_metal.m | - | s_pso_last | file_static | keep | - | index into kept pipeline cache, key-checked |
| src/d3d/nv2a_pb_metal.m | pipeline | said | local_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_metal.m | pipeline | next | local_static | keep | - | round-robin eviction index |
| src/d3d/nv2a_pb_metal.m | - | s_stage | file_static | reset | renderer.metal | stages decoded from guest texture registers |
| src/d3d/nv2a_pb_metal.m | - | s_tc | file_static | reset | renderer.metal | textures keyed by guest offset/format, from guest memory |
| src/d3d/nv2a_pb_metal.m | tex_cache_n | n | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_metal.m | - | s_tc_clock | file_static | keep | - | monotonic texture LRU clock |
| src/d3d/nv2a_pb_metal.m | - | s_texels | file_static | keep | - | host decode scratch buffer |
| src/d3d/nv2a_pb_metal.m | - | s_texels_cap | file_static | keep | - | scratch buffer capacity |
| src/d3d/nv2a_pb_metal.m | - | s_black | file_static | keep | - | fixed black texture |
| src/d3d/nv2a_pb_metal.m | - | s_magenta | file_static | keep | - | fixed magenta texture |
| src/d3d/nv2a_pb_metal.m | - | s_black_cube | file_static | keep | - | fixed black cube texture |
| src/d3d/nv2a_pb_metal.m | no_mips | off | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_metal.m | memo_on | on | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_metal.m | build_stages | memo | local_static | reset | renderer.metal | keyed on tex-regs gen/contig; both rewind at reset |
| src/d3d/nv2a_pb_metal.m | tex_upload_body | said | local_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_metal.m | tex_bind_body | said | local_static | keep | - | log-once guard table |
| src/d3d/nv2a_pb_metal.m | border_colour | said | local_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_metal.m | - | s_smp | file_static | keep | - | sampler states keyed by sampler state |
| src/d3d/nv2a_pb_metal.m | - | s_smp_n | file_static | keep | - | sampler cache fill count |
| src/d3d/nv2a_pb_metal.m | sampler | point | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_metal.m | sampler | next | local_static | keep | - | round-robin eviction index |
| src/d3d/nv2a_pb_metal.m | - | s_compile_errors | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_vs_compiled | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_fs_compiled | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | compile_fn | nonce_read | local_static | keep | - | env read guard |
| src/d3d/nv2a_pb_metal.m | compile_fn | nonce | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_metal.m | compile_fn | dump | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_metal.m | compile_fn | dump_said | local_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_metal.m | - | s_vs | file_static | keep | - | VS functions keyed by vertex program content hash |
| src/d3d/nv2a_pb_metal.m | vs_cache | parsed | local_static | keep | - | scratch parse buffer |
| src/d3d/nv2a_pb_metal.m | vs_cache | msl | local_static | keep | - | scratch MSL buffer |
| src/d3d/nv2a_pb_metal.m | vs_cache | memo | local_static | reset | renderer.metal | keyed on prog_dwords/start; pgraph counter rewinds at reset |
| src/d3d/nv2a_pb_metal.m | - | s_fs | file_static | keep | - | FS functions keyed by combiner-state content |
| src/d3d/nv2a_pb_metal.m | - | s_fs_n | file_static | keep | - | FS cache fill count |
| src/d3d/nv2a_pb_metal.m | - | s_fs_comb | file_static | keep | - | set by fs_select before every batch's binds |
| src/d3d/nv2a_pb_metal.m | - | s_fs_w | file_static | keep | - | set by fs_select before every batch's binds |
| src/d3d/nv2a_pb_metal.m | fs_basic | tried | local_static | keep | - | snap FS built once |
| src/d3d/nv2a_pb_metal.m | fs_basic | fn | local_static | keep | - | snap FS from fixed source |
| src/d3d/nv2a_pb_metal.m | fs_select_body | msl | local_static | keep | - | scratch MSL buffer |
| src/d3d/nv2a_pb_metal.m | fs_select_body | memo | local_static | keep | - | keyed by combiner register content, kept s_fs |
| src/d3d/nv2a_pb_metal.m | - | s_dss | file_static | keep | - | depth-stencil states keyed by state key |
| src/d3d/nv2a_pb_metal.m | - | s_dss_n | file_static | keep | - | DSS cache fill count |
| src/d3d/nv2a_pb_metal.m | - | s_self | file_static | reset | renderer.metal | self-sample copy of a guest frame target |
| src/d3d/nv2a_pb_metal.m | rt_texture | off | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_metal.m | rt_texture | said | local_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_metal.m | aspect_trace | on | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_metal.m | aspect_log | set | local_static | keep | - | log-once key set |
| src/d3d/nv2a_pb_metal.m | aspect_log | used | local_static | keep | - | log-once key count |
| src/d3d/nv2a_pb_metal.m | aspect_log_flat | set | local_static | keep | - | log-once key set |
| src/d3d/nv2a_pb_metal.m | aspect_log_flat | used | local_static | keep | - | log-once key count |
| src/d3d/nv2a_pb_metal.m | aspect_log_post | set | local_static | keep | - | log-once key set |
| src/d3d/nv2a_pb_metal.m | aspect_log_post | used | local_static | keep | - | log-once key count |
| src/d3d/nv2a_pb_metal.m | metal_on_draw | idx | local_static | keep | - | scratch index buffer |
| src/d3d/nv2a_pb_metal.m | metal_on_draw | idx_cap | local_static | keep | - | scratch capacity |
| src/d3d/nv2a_pb_metal.m | metal_on_draw | hxy | local_static | keep | - | scratch hor+ vertex xy |
| src/d3d/nv2a_pb_metal.m | metal_on_draw | hxy_cap | local_static | keep | - | scratch capacity |
| src/d3d/nv2a_pb_metal.m | metal_on_draw | bump_trace | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_metal.m | metal_on_draw | bias_enc | local_static | keep | - | encoder serial of the last setDepthBias; s_enc_serial is monotonic |
| src/d3d/nv2a_pb_metal.m | metal_on_draw | bias_set | local_static | keep | - | last depth bias on that encoder |
| src/d3d/nv2a_pb_metal.m | - | s_shown_w | file_static | keep | - | last present extent, host display |
| src/d3d/nv2a_pb_metal.m | - | s_shown_h | file_static | keep | - | last present extent, host display |
| src/d3d/nv2a_pb_metal.m | write_back | row | local_static | keep | - | scratch row buffer |
| src/d3d/nv2a_pb_metal.m | write_back | row_cap | local_static | keep | - | scratch capacity |
| src/d3d/nv2a_pb_metal.m | host_readback | px | local_static | keep | - | scratch readback buffer |
| src/d3d/nv2a_pb_metal.m | host_readback | cap | local_static | keep | - | scratch capacity |
| src/d3d/nv2a_pb_metal.m | write_back_scaled | row | local_static | keep | - | scratch row buffer |
| src/d3d/nv2a_pb_metal.m | write_back_scaled | row_cap | local_static | keep | - | scratch capacity |
| src/d3d/nv2a_pb_metal.m | - | s_guard_always | file_static | keep | - | debug fb guard saw a title read |
| src/d3d/nv2a_pb_metal.m | - | s_guard | file_static | reset | renderer.metal | PROT_NONE span over guest fb pages; unprotect before clear |
| src/d3d/nv2a_pb_metal.m | guard_flip | on | local_static | keep | - | cached env read; handler installed once |
| src/d3d/nv2a_pb_metal.m | guard_flip | said_write | local_static | keep | - | log-once guard |
| src/d3d/nv2a_pb_metal.m | - | s_qpool | file_static | keep | - | host query pool; pending ones return via s_occ |
| src/d3d/nv2a_pb_metal.m | - | s_qfree | file_static | keep | - | query free list; refilled when s_occ resets |
| src/d3d/nv2a_pb_metal.m | - | s_qmade | file_static | keep | - | queries made total |
| src/d3d/nv2a_pb_metal.m | - | s_vis_used | file_static | reset | renderer.metal | visibility slots held by pending queries |
| src/d3d/nv2a_pb_metal.m | - | s_vis_next | file_static | reset | renderer.metal | next visibility slot to hand out |
| src/d3d/nv2a_pb_metal.m | - | s_retire | file_static | reset | renderer.metal | slots awaiting retire after a wait |
| src/d3d/nv2a_pb_metal.m | - | s_nretire | file_static | reset | renderer.metal | count of slots awaiting retire |
| src/d3d/nv2a_pb_metal.m | - | s_occ | file_static | reset | renderer.metal | pending occlusion reports at guest VAs |
| src/d3d/nv2a_pb_metal.m | - | s_occ_mode | file_static | keep | - | cached RENV_METAL_OCC read |
| src/d3d/nv2a_pb_metal.m | - | s_occ_ovf | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | - | s_report_commits | file_static | keep | - | stats counter |
| src/d3d/nv2a_pb_metal.m | flip_log | on | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_metal.m | flip_log | p_draws | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | p_prog | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | p_skip | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | p_cull | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | p_clears | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | p_tex | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | p_up | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | p_chg | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | p_re | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | p_fl | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | p_occ | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | p_vis | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | p_late | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | p_super | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | p_sync | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | p_ovf | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | p_set | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | p_stale | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | p_first | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | p_cup | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | flip_log | zt | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_metal.m | flip_log | p_stretch | local_static | keep | - | flip log previous totals |
| src/d3d/nv2a_pb_metal.m | prof_flip_end | p_wb | local_static | keep | - | profiler previous totals |
| src/d3d/nv2a_pb_metal.m | prof_flip_end | p_ds | local_static | keep | - | profiler previous totals |
| src/d3d/nv2a_pb_metal.m | prof_flip_end | p_pa | local_static | keep | - | profiler previous totals |
| src/d3d/nv2a_pb_metal.m | host_dump_due | prefix | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_metal.m | host_dump_due | every | local_static | keep | - | cached env read |
| src/d3d/nv2a_pb_metal.m | - | s_dbg_pso | file_static | keep | - | debug-draw pipelines |
| src/d3d/nv2a_pb_metal.m | - | s_dbg_dss | file_static | keep | - | debug-draw depth states |
| src/d3d/nv2a_pb_metal.m | - | s_dbg_vb | file_static | keep | - | debug-draw vertex buffer |
| src/d3d/nv2a_pb_metal.m | - | s_dbg_vb_cap | file_static | keep | - | debug-draw buffer capacity |
| src/d3d/nv2a_pb_metal.m | - | s_dbg_failed | file_static | keep | - | debug-draw build failed flag |
| src/d3d/nv2a_pb_metal.m | metal_flip | px | local_static | keep | - | scratch readback buffer |
| src/d3d/nv2a_pb_metal.m | metal_flip | cap | local_static | keep | - | scratch capacity |
| src/d3d/nv2a_pb_metal.m | - | s_mag | file_static | keep | - | the tex_mag trace, a debug figure keyed by content hash |
| src/d3d/nv2a_pb_metal.m | - | s_mag_cur | file_static | keep | - | the tex_mag trace, a debug figure keyed by content hash |
| src/d3d/nv2a_pb_metal.m | - | s_mag_state | file_static | keep | - | the tex_mag trace, a debug figure keyed by content hash |
| src/d3d/nv2a_pb_metal.m | - | s_fs_mag | file_static | keep | - | the tex_mag trace, a debug figure keyed by content hash |
| src/d3d/nv2a_pb_metal.m | - | s_up_scale | file_static | keep | - | the upload just made, read back by its caller |
| src/d3d/nv2a_pb_metal.m | - | s_up_levels | file_static | keep | - | the upload just made, read back by its caller |
| src/d3d/nv2a_pb_metal.m | - | s_tex_last | file_static | keep | - | the entry tex_bind just bound, set again on each bind |
| src/d3d/nv2a_pb_metal.m | - | s_stage_pack | file_static | keep | - | the entry each stage just bound, set again on each draw |
| src/d3d/nv2a_pb_metal.m | - | s_stage_hash | file_static | keep | - | the entry each stage just bound, set again on each draw |

## Devices: APU, DSP, audio, input, USB, video, platform

### src/apu/apu_core.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/apu/apu_core.c | - | g_apu_ram_ptr | global | keep | - | host base of guest RAM; mapping kept in place |
| src/apu/apu_core.c | - | g_state | global | keep | - | device struct pointer; apu hook resets contents |
| src/apu/apu_core.c | - | g_mixer_voices | file_static | reset | dsound | holds pcm pointers into guest DSound buffers |
| src/apu/apu_core.c | - | g_mixer_active_count | file_static | reset | dsound | HLE mixer voice count |
| src/apu/apu_core.c | - | g_mixer_cs | file_static | keep | - | host lock |
| src/apu/apu_core.c | - | g_mixer_initialized | once | keep | - | no rerun: inits mixer lock; hook clears voices |
| src/apu/apu_core.c | - | g_dbg | global | reset | apu | per-voice debug holds guest ebo/cbo/ba |
| src/apu/apu_core.c | - | g_dbg_cache | global | keep | - | declared, never written |
| src/apu/apu_core.c | - | g_dbg_voice_monitor | global | keep | - | env apu_solo |
| src/apu/apu_core.c | - | g_dbg_muted_voices | global | keep | - | never written |
| src/apu/apu_core.c | - | g_audio_muted | global | keep | - | host mute setting |
| src/apu/apu_core.c | apu_mmio_trace | on | local_static | keep | - | host log guard or debug state |
| src/apu/apu_core.c | - | s_irq_line | file_static | reset | apu | APU IRQ line level |
| src/apu/apu_core.c | - | apu_irq_stats | global | keep | - | stats |
| src/apu/apu_core.c | - | s_irq_trace | file_static | keep | - | env trace flag |
| src/apu/apu_core.c | - | s_irq_next_ns | file_static | reset | apu | deadline on APU clock; apu_diag_reset zeroes it |
| src/apu/apu_core.c | apu_call_isr | n | local_static | keep | - | host log guard or debug state |
| src/apu/apu_core.c | apu_deliver_irq | tib_ready | once | reset | apu | rerun: TLS flag for a guest-memory TIB |
| src/apu/apu_core.c | apu_deliver_irq | held_off | local_static | reset | apu | held IRQ the guest has not seen |
| src/apu/apu_core.c | - | g_test_tone | file_static | keep | - | env test tone |
| src/apu/apu_core.c | - | g_waveout | file_static | keep | - | host waveOut device |
| src/apu/apu_core.c | - | g_hs | file_static | keep | - | host stream stats |
| src/apu/apu_core.c | waveout_submit | fill | local_static | keep | - | host stream staging |
| src/apu/apu_core.c | mcpx_apu_pace_step | shown | local_static | keep | - | host log guard or debug state |
| src/apu/apu_core.c | apu_mixer_play | play_log_count | local_static | keep | - | host log guard or debug state |

### src/apu/apu_dsp.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/apu/apu_dsp.c | - | s_dsp_ack | file_static | keep | - | env ack address list |
| src/apu/apu_dsp.c | - | s_dsp_ack_count | file_static | keep | - | env cache or trace counter |
| src/apu/apu_dsp.c | - | s_dsp_ack_auto | file_static | keep | - | env cache or trace counter |
| src/apu/apu_dsp.c | - | s_dsp_ack_auto_page | file_static | reset | apu | guest page of auto ack |
| src/apu/apu_dsp.c | - | s_gp_in_reset | file_static | reset | apu | GP DSP reset latch |
| src/apu/apu_dsp.c | mcpx_apu_mixdown_mode | mode | local_static | keep | - | env cache or trace counter |
| src/apu/apu_dsp.c | - | s_boot_rst | file_static | reset | apu | RST bits seen in MCPX window |
| src/apu/apu_dsp.c | - | s_boots | file_static | keep | - | env cache or trace counter |
| src/apu/apu_dsp.c | mcpx_apu_dsp_ack_poll | shown | local_static | keep | - | env cache or trace counter |
| src/apu/apu_dsp.c | dsp_bin_stats | on | local_static | keep | - | env cache or trace counter |
| src/apu/apu_dsp.c | dsp_bin_stats | sq | local_static | keep | - | env cache or trace counter |
| src/apu/apu_dsp.c | dsp_bin_stats | peak | local_static | keep | - | env cache or trace counter |
| src/apu/apu_dsp.c | dsp_bin_stats | frames | local_static | keep | - | env cache or trace counter |
| src/apu/apu_dsp.c | dsp_bin_stats | secs | local_static | keep | - | env cache or trace counter |

### src/apu/apu_dsp_run.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/apu/apu_dsp_run.c | - | s | file_static | reset | dsp | DSP cores; keep workers and env fields |
| src/apu/apu_dsp_run.c | sg_rw | logs | local_static | keep | - | log guard |

### src/apu/apu_lockstat.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/apu/apu_lockstat.c | - | apu_lockstat_on_ | global | keep | - | lock contention stats |
| src/apu/apu_lockstat.c | - | s_hold | file_static | keep | - | lock contention stats |
| src/apu/apu_lockstat.c | - | s_wait | file_static | keep | - | lock contention stats |
| src/apu/apu_lockstat.c | - | s_stage | file_static | keep | - | lock contention stats |
| src/apu/apu_lockstat.c | - | s_hold_start | file_static | keep | - | lock contention stats |
| src/apu/apu_lockstat.c | - | s_se_n | file_static | keep | - | lock contention stats |
| src/apu/apu_lockstat.c | - | s_se_ns | file_static | keep | - | lock contention stats |
| src/apu/apu_lockstat.c | - | s_se_max | file_static | keep | - | lock contention stats |
| src/apu/apu_lockstat.c | - | s_pace_waits | file_static | keep | - | lock contention stats |
| src/apu/apu_lockstat.c | - | s_t0 | file_static | keep | - | lock contention stats |
| src/apu/apu_lockstat.c | - | s_last | file_static | keep | - | lock contention stats |
| src/apu/apu_lockstat.c | - | s_text | file_static | keep | - | lock contention stats |
| src/apu/apu_lockstat.c | - | s_text_ready | once | keep | - | no rerun: report text handoff flag |
| src/apu/apu_lockstat.c | - | s_freq | file_static | keep | - | lock contention stats |

### src/apu/apu_mmio_hook.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/apu/apu_mmio_hook.c | - | g_apu_state | global | keep | - | device struct pointer; contents reset by apu |
| src/apu/apu_mmio_hook.c | - | g_apu_mmio_read_count | file_static | keep | - | MMIO trace stats |
| src/apu/apu_mmio_hook.c | - | g_apu_mmio_write_count | file_static | keep | - | MMIO trace stats |
| src/apu/apu_mmio_hook.c | - | g_apu_mmio_decode_fail | file_static | keep | - | MMIO trace stats |
| src/apu/apu_mmio_hook.c | - | s_last_wval | file_static | keep | - | MMIO trace stats |
| src/apu/apu_mmio_hook.c | - | s_trace | file_static | keep | - | MMIO trace stats |
| src/apu/apu_mmio_hook.c | - | s_hist_r | file_static | keep | - | MMIO trace stats |
| src/apu/apu_mmio_hook.c | - | s_hist_w | file_static | keep | - | MMIO trace stats |
| src/apu/apu_mmio_hook.c | - | s_hist_lastw | file_static | keep | - | MMIO trace stats |
| src/apu/apu_mmio_hook.c | - | s_tot_r | file_static | keep | - | MMIO trace stats |
| src/apu/apu_mmio_hook.c | - | s_tot_w | file_static | keep | - | MMIO trace stats |
| src/apu/apu_mmio_hook.c | - | s_direct | file_static | keep | - | MMIO trace stats |
| src/apu/apu_mmio_hook.c | - | s_handler_ticks | file_static | keep | - | MMIO trace stats |
| src/apu/apu_mmio_hook.c | - | s_reporter_started | once | keep | - | no rerun: trace reporter thread keeps running |
| src/apu/apu_mmio_hook.c | - | g_apu_hook_bench_active | global | keep | - | bench flag |
| src/apu/apu_mmio_hook.c | - | s_qpf | file_static | constant | - | host counter frequency |
| src/apu/apu_mmio_hook.c | apu_trace_report | prev_r | local_static | keep | - | MMIO trace stats |
| src/apu/apu_mmio_hook.c | apu_trace_report | prev_w | local_static | keep | - | MMIO trace stats |
| src/apu/apu_mmio_hook.c | apu_trace_report | prev_t | local_static | keep | - | MMIO trace stats |
| src/apu/apu_mmio_hook.c | apu_trace_tally | n | local_static | keep | - | MMIO trace stats |
| src/apu/apu_mmio_hook.c | - | s_handler_ns | file_static | keep | - | MMIO trace stats |
| src/apu/apu_mmio_hook.c | apu_hook_trace_start | started | once | keep | - | no rerun: trace reporter thread keeps running |

### src/apu/apu_sdl.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/apu/apu_sdl.c | - | g_dev | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_sdl.c | - | g_buf_samples | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_sdl.c | - | g_num_bufs | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_sdl.c | - | g_initialized | once | keep | - | no rerun: host audio device stays |
| src/apu/apu_sdl.c | - | g_audio_opened | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_sdl.c | - | g_frames_written | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_sdl.c | - | g_samples_queued | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_sdl.c | - | g_underruns | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_sdl.c | - | g_dry | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_sdl.c | - | g_underrun_logged_ms | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_sdl.c | - | g_virtual | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_sdl.c | - | g_vclock | file_static | keep | - | host audio device and stream stay |

### src/apu/apu_shim.h

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/apu/apu_shim.h | - | address_space_memory_dummy | file_static | constant | - | QEMU shim placeholder, never written |
| src/apu/apu_shim.h | src_callback_new | dummy_src | local_static | constant | - | QEMU shim placeholder, never written |
| src/apu/apu_shim.h | - | g_config | file_static | constant | - | QEMU shim placeholder, never written |

### src/apu/apu_vp.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/apu/apu_vp.c | fe_method | s_dropped_logged | local_static | keep | - | trace or dump state, env gated |
| src/apu/apu_vp.c | - | s_trace_buf | file_static | keep | - | trace or dump state, env gated |
| src/apu/apu_vp.c | - | s_trace_len | file_static | keep | - | trace or dump state, env gated |
| src/apu/apu_vp.c | - | s_trace_dropped | file_static | keep | - | trace or dump state, env gated |
| src/apu/apu_vp.c | - | s_vstat | file_static | reset | apu | per-voice stats keyed by guest buffer address |
| src/apu/apu_vp.c | - | s_vstat_on | file_static | keep | - | trace or dump state, env gated |
| src/apu/apu_vp.c | - | s_vdump_voice | file_static | keep | - | trace or dump state, env gated |
| src/apu/apu_vp.c | - | s_vdump | file_static | keep | - | trace or dump state, env gated |
| src/apu/apu_vp.c | - | s_vdump_all | file_static | keep | - | trace or dump state, env gated |
| src/apu/apu_vp.c | - | s_vdump_failed | file_static | keep | - | trace or dump state, env gated |
| src/apu/apu_vp.c | - | s_vdump_prefix | file_static | keep | - | trace or dump state, env gated |
| src/apu/apu_vp.c | - | s_vstat_frames | file_static | keep | - | trace or dump state, env gated |
| src/apu/apu_vp.c | - | s_vstat_secs | file_static | keep | - | trace or dump state, env gated |
| src/apu/apu_vp.c | - | s_ring | file_static | reset | apu | ring monitors keyed by guest ba |
| src/apu/apu_vp.c | - | s_ring_on | file_static | keep | - | trace or dump state, env gated |
| src/apu/apu_vp.c | - | s_ring_logged | file_static | keep | - | trace or dump state, env gated |
| src/apu/apu_vp.c | - | s_ring_frames | file_static | keep | - | trace or dump state, env gated |
| src/apu/apu_vp.c | - | s_ring_secs | file_static | keep | - | trace or dump state, env gated |
| src/apu/apu_vp.c | voice_get_samples | warned | local_static | keep | - | trace or dump state, env gated |
| src/apu/apu_vp.c | voice_process | hrtf_on | local_static | keep | - | trace or dump state, env gated |
| src/apu/apu_vp.c | - | s_pos_frames | file_static | keep | - | trace or dump state, env gated |
| src/apu/apu_vp.c | mcpx_apu_vp_frame | queue_full_warned | local_static | keep | - | trace or dump state, env gated |

### src/apu/apu_wav.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/apu/apu_wav.c | - | s_lock | file_static | keep | - | debug WAV capture file |
| src/apu/apu_wav.c | - | s_lock_init | once | keep | - | no rerun: host lock init |
| src/apu/apu_wav.c | - | s_mutex | file_static | keep | - | debug WAV capture file |
| src/apu/apu_wav.c | - | s_fp | file_static | keep | - | debug WAV capture file |
| src/apu/apu_wav.c | - | s_frames | file_static | keep | - | debug WAV capture file |
| src/apu/apu_wav.c | - | s_cap | file_static | keep | - | debug WAV capture file |
| src/apu/apu_wav.c | - | s_next_refresh | file_static | keep | - | debug WAV capture file |
| src/apu/apu_wav.c | - | s_hooks | file_static | keep | - | debug WAV capture file |

### src/apu/apu_xaudio2.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/apu/apu_xaudio2.c | - | g_xa2_buf_samples | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_xaudio2.c | - | g_xa2_num_bufs | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_xaudio2.c | - | g_xa2 | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_xaudio2.c | - | g_xa2_master | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_xaudio2.c | - | g_xa2_source | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_xaudio2.c | - | g_xa2_bufs | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_xaudio2.c | - | g_xa2_next_buf | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_xaudio2.c | - | g_xa2_fill | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_xaudio2.c | - | g_xa2_underruns | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_xaudio2.c | - | g_xa2_dry | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_xaudio2.c | - | g_xa2_underrun_logged | file_static | keep | - | host audio device and stream stay |
| src/apu/apu_xaudio2.c | - | g_xa2_initialized | once | keep | - | no rerun: host audio device stays |
| src/apu/apu_xaudio2.c | - | g_xa2_frames_written | file_static | keep | - | host audio device and stream stay |

### src/audio/dsound_device.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/audio/dsound_device.c | ds_CreateSoundBuffer | create_log_count | local_static | keep | - | log counter |
| src/audio/dsound_device.c | - | g_dsound | file_static | constant | - | holds only a vtable pointer |

### src/input/host_open.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/input/host_open.c | - | s_slot | file_static | reset | input | pending menu open request, drained in band 4 |
| src/input/host_open.c | - | s_path | file_static | reset | input | pending menu open path, drained in band 4 |

### src/input/input_core.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/input/input_core.c | - | g_be | file_static | keep | - | host pad backend; pads stay connected |
| src/input/input_core.c | - | g_inited | once | keep | - | no rerun: host backend up; pads stay connected |
| src/input/input_core.c | - | g_connected | file_static | keep | - | host pad backend; pads stay connected |
| src/input/input_core.c | - | g_last_probe | file_static | keep | - | host pad backend; pads stay connected |
| src/input/input_core.c | - | g_probed | file_static | keep | - | host pad backend; pads stay connected |
| src/input/input_core.c | - | g_rumbling | file_static | reset | input | motors the guest turned on |
| src/input/input_core.c | - | g_extra | file_static | keep | - | host pad backend; pads stay connected |
| src/input/input_core.c | - | g_filter | file_static | keep | - | host pad backend; pads stay connected |
| src/input/input_core.c | - | g_seen | file_static | keep | - | host pad backend; pads stay connected |
| src/input/input_core.c | - | g_glyphs | file_static | keep | - | host pad backend; pads stay connected |
| src/input/input_core.c | - | g_raw | file_static | keep | - | host pad backend; pads stay connected |
| src/input/input_core.c | - | g_raw_lock | file_static | keep | - | host pad backend; pads stay connected |
| src/input/input_core.c | - | g_cfg | file_static | keep | - | host pad backend; pads stay connected |
| src/input/input_core.c | xbox_InputInit | atexit_done | local_static | keep | - | host pad backend; pads stay connected |
| src/input/input_core.c | - | g_last | file_static | keep | - | host pad backend; pads stay connected |
| src/input/input_core.c | - | g_last_rc | file_static | keep | - | host pad backend; pads stay connected |
| src/input/input_core.c | - | g_last_lock | file_static | keep | - | host pad backend; pads stay connected |
| src/input/input_core.c | - | g_neutral | file_static | keep | - | host pad backend; pads stay connected |
| src/input/input_core.c | - | g_neutral_bump | file_static | keep | - | monotonic packet number bump |

### src/input/kbm_script.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/input/kbm_script.c | - | s_ev | file_static | keep | - | automation script; host side |
| src/input/kbm_script.c | - | s_n | file_static | keep | - | automation script; host side |
| src/input/kbm_script.c | - | s_cap | file_static | keep | - | automation script; host side |
| src/input/kbm_script.c | - | s_allow | file_static | keep | - | automation script; host side |
| src/input/kbm_script.c | - | s_tried | file_static | keep | - | automation script; host side |
| src/input/kbm_script.c | - | s_polls | file_static | keep | - | automation script; host side |
| src/input/kbm_script.c | - | s_t0 | file_static | keep | - | automation script; host side |
| src/input/kbm_script.c | - | s_started | once | keep | - | no rerun: automation script runs once |

### src/input/keyboard.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/input/keyboard.c | - | s_key_down | file_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | - | s_return_main | file_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | - | s_return_kp | file_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | - | s_shortcut | file_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | xbox_InputTraceEdge | n | local_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | - | s_bind | file_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | - | s_bind_ready | once | keep | - | no rerun: bindings stay loaded |
| src/input/keyboard.c | - | s_ramp_ms | file_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | - | s_walk_code | file_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | - | s_walk_scale | file_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | - | s_kb_enable | file_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | - | s_square_left | file_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | - | s_layout | file_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | compose | packet | local_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | compose | s_dir | local_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | compose | s_since | local_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | - | s_pending | file_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | - | s_live | file_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | - | s_retired | file_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | - | s_drop | file_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | xbox_InputKeyboardState | compiled | local_static | keep | - | host keyboard state and bindings |
| src/input/keyboard.c | xbox_InputKeyboardState | ready | once | keep | - | no rerun: host key tables stay |
| src/input/keyboard.c | - | s_clock | file_static | keep | - | host clock source set by the options call |

### src/input/mouse.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/input/mouse.c | - | s_optbuf | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_optidx | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_enable | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_dx | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_dy | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_wheel_q | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_btn | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_focus | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_suspend | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_key_free | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_clicked | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_forced | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_closed | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_windowed | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_activate_click | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_focus_armed | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_focus_ms | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_captured | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_lock | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_wheel_held | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_wheel_until | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_cursor_hide | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_divert | file_static | keep | - | host mouse state |
| src/input/mouse.c | xbox_InputMouseLook | traced | local_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_applied | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_raw_seen | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_moves | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_warp_stuck | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_path_logged | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_abs_x | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_abs_y | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_have_abs | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_wheel_rem | file_static | keep | - | host mouse state |
| src/input/mouse.c | - | s_cur_hidden | file_static | keep | - | host mouse state |
| src/input/mouse.c | xbox_InputMouseWin32Sync | hooked | local_static | keep | - | host mouse state |

### src/input/mouse_sdl.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/input/mouse_sdl.c | - | s_relative | file_static | keep | - | host mouse state |
| src/input/mouse_sdl.c | - | s_hidden | file_static | keep | - | host mouse state |
| src/input/mouse_sdl.c | - | s_rem_x | file_static | keep | - | host mouse state |
| src/input/mouse_sdl.c | - | s_rem_y | file_static | keep | - | host mouse state |
| src/input/mouse_sdl.c | - | s_rem_wheel | file_static | keep | - | host mouse state |

### src/input/pad_map.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/input/pad_map.c | - | g_deadzone | file_static | keep | - | env deadzone |

### src/input/pointer.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/input/pointer.c | - | s_lock | file_static | keep | - | host pointer state |
| src/input/pointer.c | - | s_x | file_static | keep | - | host pointer state |
| src/input/pointer.c | - | s_y | file_static | keep | - | host pointer state |
| src/input/pointer.c | - | s_buttons | file_static | keep | - | host pointer state |
| src/input/pointer.c | - | s_seq | file_static | keep | - | host pointer state |
| src/input/pointer.c | - | s_wheel | file_static | keep | - | host pointer state |
| src/input/pointer.c | - | s_presses | file_static | keep | - | host pointer state |
| src/input/pointer.c | - | s_press_x | file_static | keep | - | host pointer state |
| src/input/pointer.c | - | s_press_y | file_static | keep | - | host pointer state |
| src/input/pointer.c | - | s_seen | file_static | keep | - | host pointer state |
| src/input/pointer.c | - | s_sdl_rem | file_static | keep | - | host pointer state |
| src/input/pointer.c | - | s_win_rem | file_static | keep | - | host pointer state |

### src/input/text_input.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/input/text_input.c | - | s_ring | file_static | keep | - | menu text input ring |
| src/input/text_input.c | - | s_tag | file_static | keep | - | menu text input ring |
| src/input/text_input.c | - | s_head | file_static | keep | - | menu text input ring |
| src/input/text_input.c | - | s_tail | file_static | keep | - | menu text input ring |
| src/input/text_input.c | - | s_dropped | file_static | keep | - | menu text input ring |
| src/input/text_input.c | - | s_seq | file_static | keep | - | menu text input ring |
| src/input/text_input.c | - | s_state | file_static | keep | - | menu text input ring |
| src/input/text_input.c | - | s_rect | file_static | keep | - | menu text input ring |
| src/input/text_input.c | - | s_dropped_at_begin | file_static | keep | - | menu text input ring |
| src/input/text_input.c | - | s_high | file_static | keep | - | menu text input ring |
| src/input/text_input.c | - | s_high_state | file_static | keep | - | menu text input ring |
| src/input/text_input.c | - | s_lead | file_static | keep | - | menu text input ring |
| src/input/text_input.c | - | s_lead_state | file_static | keep | - | menu text input ring |

### src/input/text_input_sdl.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/input/text_input_sdl.c | - | s_applied | file_static | keep | - | host text input state |

### src/input/xinput_device.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/input/xinput_device.c | - | g_xi_ex | file_static | constant | - | resolved host function pointer |
| src/input/xinput_device.c | - | g_xi_ex_done | file_static | constant | - | guard for resolved function pointer |
| src/input/xinput_device.c | - | g_slot | file_static | keep | - | host pads; rumble gate zeroed by input hook |
| src/input/xinput_device.c | - | g_slot_lock | file_static | keep | - | host pad slots stay connected |
| src/input/xinput_device.c | - | g_rescan | file_static | keep | - | host pad slots stay connected |
| src/input/xinput_device.c | - | g_last_scan | file_static | keep | - | host pad slots stay connected |
| src/input/xinput_device.c | - | g_wanted | file_static | keep | - | host pad slots stay connected |
| src/input/xinput_device.c | - | g_ready | once | keep | - | no rerun: SDL gamepad subsystem stays up |
| src/input/xinput_device.c | - | g_ready_cv | file_static | keep | - | host pad slots stay connected |
| src/input/xinput_device.c | - | g_main_pump | file_static | keep | - | host pad slots stay connected |
| src/input/xinput_device.c | - | g_packet | file_static | keep | - | host pad slots stay connected |

### src/platform/host_prof.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/platform/host_prof.c | - | s_tab | file_static | keep | - | host profiler state |
| src/platform/host_prof.c | - | s_slot | file_static | keep | - | host profiler state |
| src/platform/host_prof.c | - | s_nslots | file_static | keep | - | host profiler state |
| src/platform/host_prof.c | - | s_samples | file_static | keep | - | host profiler state |
| src/platform/host_prof.c | - | s_dropped | file_static | keep | - | host profiler state |
| src/platform/host_prof.c | - | s_rounds | file_static | keep | - | host profiler state |
| src/platform/host_prof.c | - | s_hz | file_static | keep | - | host profiler state |
| src/platform/host_prof.c | - | s_out | file_static | keep | - | host profiler state |
| src/platform/host_prof.c | - | s_running | file_static | keep | - | host profiler state |
| src/platform/host_prof.c | - | s_stop | file_static | keep | - | host profiler state |
| src/platform/host_prof.c | - | s_dumped | file_static | keep | - | host profiler state |
| src/platform/host_prof.c | - | s_in_round | file_static | keep | - | host profiler state |
| src/platform/host_prof.c | - | s_all_threads | file_static | keep | - | host profiler state |
| src/platform/host_prof.c | - | s_th | file_static | keep | - | host profiler state |
| src/platform/host_prof.c | - | s_nth | file_static | keep | - | host profiler state |
| src/platform/host_prof.c | thread_name | get_desc | local_static | constant | - | resolved host function pointer |
| src/platform/host_prof.c | thread_name | resolved | local_static | constant | - | guard for resolved function pointer |
| src/platform/host_prof.c | - | s_delay_ms | file_static | keep | - | host profiler state |
| src/platform/host_prof.c | host_prof_dump | mbase | local_static | keep | - | host profiler state |
| src/platform/host_prof.c | host_prof_dump | mpath | local_static | keep | - | host profiler state |

### src/platform/host_time.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/platform/host_time.c | xbox_HostNowNs | freq | local_static | constant | - | host counter frequency |
| src/platform/host_time.c | thread_timer | timer | local_static | keep | - | host timer state |
| src/platform/host_time.c | thread_timer | timer_failed | local_static | keep | - | host timer state |
| src/platform/host_time.c | - | s_ntdll_once | once | constant | - | no rerun: resolves ntdll delay function |
| src/platform/host_time.c | - | s_under_wine | file_static | constant | - | host capability probe |
| src/platform/host_time.c | - | s_nt_delay | file_static | constant | - | resolved host function pointer |
| src/platform/host_time.c | - | s_nap_mode | file_static | keep | - | host timer state |
| src/platform/host_time.c | - | s_nap_no_timer | file_static | keep | - | host timer state |
| src/platform/host_time.c | - | s_timer_wake | file_static | keep | - | host timer state |
| src/platform/host_time.c | - | s_timer_wake_once | once | keep | - | no rerun: host wake event stays |
| src/platform/host_time.c | xbox_HostTimerSleepNs | timer | local_static | keep | - | host timer state |
| src/platform/host_time.c | xbox_HostTimerSleepNs | timer_failed | local_static | keep | - | host timer state |
| src/platform/host_time.c | - | s_timer_wake_lock | file_static | keep | - | host timer state |
| src/platform/host_time.c | - | s_timer_wake_cv | file_static | keep | - | host timer state |
| src/platform/host_time.c | - | s_timer_wake_pending | file_static | keep | - | host timer state |

### src/platform/mem_stats.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/platform/mem_stats.c | - | s_gpu | file_static | keep | - | memory stats |
| src/platform/mem_stats.c | - | s_gpu_peak | file_static | keep | - | memory stats |
| src/platform/mem_stats.c | - | s_gpu_nl | file_static | keep | - | memory stats |
| src/platform/mem_stats.c | - | s_gpu_nl_peak | file_static | keep | - | memory stats |
| src/platform/mem_stats.c | - | s_gpu_src | file_static | keep | - | memory stats |
| src/platform/mem_stats.c | - | s_ramcache | file_static | keep | - | memory stats |
| src/platform/mem_stats.c | - | s_ramcache_peak | file_static | keep | - | memory stats |
| src/platform/mem_stats.c | - | s_said | file_static | keep | - | memory stats |
| src/platform/mem_stats.c | - | s_old_int | file_static | keep | - | memory stats |
| src/platform/mem_stats.c | - | s_old_term | file_static | keep | - | memory stats |
| src/platform/mem_stats.c | - | s_every_ms | file_static | keep | - | memory stats |
| src/platform/mem_stats.c | - | s_t0_ns | file_static | keep | - | memory stats |
| src/platform/mem_stats.c | mem_stats_start_from_env | started | once | keep | - | no rerun: stats thread keeps running |

### src/platform/recomp_env.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/platform/recomp_env.c | - | recomp_env_value_ | global | keep | - | cached env reads |
| src/platform/recomp_env.c | - | recomp_env_ready_ | global | keep | - | cached env reads |
| src/platform/recomp_env.c | - | s_notes | file_static | keep | - | cached env reads |
| src/platform/recomp_env.c | - | s_notes_len | file_static | keep | - | cached env reads |
| src/platform/recomp_env.c | - | s_notes_cap | file_static | keep | - | cached env reads |
| src/platform/recomp_env.c | - | s_state | file_static | keep | - | cached env reads |

### src/platform/recomp_exe_dir.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/platform/recomp_exe_dir.c | - | s_dir | file_static | keep | - | exe directory cache |

### src/platform/recomp_exit_hook.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/platform/recomp_exit_hook.c | - | s_hook | file_static | keep | - | host exit hooks |
| src/platform/recomp_exit_hook.c | - | s_more | file_static | keep | - | host exit hooks |
| src/platform/recomp_exit_hook.c | - | s_quit | file_static | keep | - | host exit hooks |

### src/platform/win32_compat.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/platform/win32_compat.c | - | t_last_error | tls | keep | - | POSIX Win32 emulation, host objects |
| src/platform/win32_compat.c | srw_lazy_init | init_lock | local_static | keep | - | POSIX Win32 emulation, host objects |
| src/platform/win32_compat.c | InitOnceExecuteOnce | once_lock | local_static | keep | - | POSIX Win32 emulation, host objects |
| src/platform/win32_compat.c | - | t_self_obj | tls | keep | - | POSIX Win32 emulation, host objects |
| src/platform/win32_compat.c | - | t_tid | tls | keep | - | POSIX Win32 emulation, host objects |
| src/platform/win32_compat.c | - | s_next_tid | file_static | keep | - | monotonic host thread ids |
| src/platform/win32_compat.c | - | s_files_lock | file_static | keep | - | POSIX Win32 emulation, host objects |
| src/platform/win32_compat.c | - | s_files | file_static | keep | - | host file objects; kernel hook closes guest files |
| src/platform/win32_compat.c | - | s_files_cap | file_static | keep | - | POSIX Win32 emulation, host objects |
| src/platform/win32_compat.c | - | s_files_used | file_static | keep | - | POSIX Win32 emulation, host objects |
| src/platform/win32_compat.c | - | s_files_live | file_static | keep | - | POSIX Win32 emulation, host objects |
| src/platform/win32_compat.c | - | s_process_mask | file_static | constant | - | host CPU affinity mask |
| src/platform/win32_compat.c | - | s_process_heap | file_static | keep | - | POSIX Win32 emulation, host objects |
| src/platform/win32_compat.c | exec_dropped | said | local_static | keep | - | POSIX Win32 emulation, host objects |
| src/platform/win32_compat.c | - | s_views | file_static | keep | - | host mapping views; guest RAM kept in place |
| src/platform/win32_compat.c | - | s_views_lock | file_static | keep | - | POSIX Win32 emulation, host objects |
| src/platform/win32_compat.c | anon_map_fd | map_counter | local_static | keep | - | POSIX Win32 emulation, host objects |

### src/usb/ohci.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/usb/ohci.c | - | s_hc | file_static | reset | usb | OHCI registers and lists; ohci_reset keeps base |
| src/usb/ohci.c | - | s_device_hc | file_static | keep | - | env cache or stats |
| src/usb/ohci.c | - | s_ndp | file_static | keep | - | env cache or stats |
| src/usb/ohci.c | - | s_npads | file_static | keep | - | env cache or stats |
| src/usb/ohci.c | - | s_npads_set | file_static | keep | - | env cache or stats |
| src/usb/ohci.c | - | s_enabled | file_static | keep | - | env cache or stats |
| src/usb/ohci.c | - | s_trace | file_static | keep | - | env cache or stats |
| src/usb/ohci.c | ohci_read | n | local_static | keep | - | env cache or stats |
| src/usb/ohci.c | ohci_write | n | local_static | keep | - | env cache or stats |
| src/usb/ohci.c | - | g_setup_pending_ | file_static | keep | - | env cache or stats |
| src/usb/ohci.c | - | g_setup_ | file_static | keep | - | env cache or stats |
| src/usb/ohci.c | - | g_ctrl_buf_ | file_static | keep | - | env cache or stats |
| src/usb/ohci.c | - | g_ctrl_len_ | file_static | keep | - | env cache or stats |
| src/usb/ohci.c | - | g_ctrl_sent_ | file_static | keep | - | env cache or stats |
| src/usb/ohci.c | - | s_plugged | file_static | reset | usb | ports the guest saw attached; replug at boot |
| src/usb/ohci.c | - | s_res_desc | file_static | keep | - | env cache or stats |
| src/usb/ohci.c | - | s_res_data | file_static | keep | - | env cache or stats |
| src/usb/ohci.c | - | s_n_report | file_static | keep | - | env cache or stats |
| src/usb/ohci.c | - | s_n_ctrl | file_static | keep | - | env cache or stats |
| src/usb/ohci.c | - | s_n_out_ep0 | file_static | keep | - | env cache or stats |
| src/usb/ohci.c | - | s_n_out_other | file_static | keep | - | env cache or stats |
| src/usb/ohci.c | ohci_do_td | n | local_static | keep | - | env cache or stats |
| src/usb/ohci.c | ohci_do_td | rumble | local_static | keep | - | env cache |
| src/usb/ohci.c | ohci_walk_eds | n | local_static | keep | - | env cache or stats |
| src/usb/ohci.c | ohci_run_periodic_list | stats | local_static | keep | - | env cache or stats |
| src/usb/ohci.c | ohci_run_periodic_list | last | local_static | keep | - | env cache or stats |
| src/usb/ohci.c | ohci_run_periodic_list | total | local_static | keep | - | env cache or stats |
| src/usb/ohci.c | - | s_posting_hc | file_static | reset | usb | controller posting an IRQ |
| src/usb/ohci.c | ohci_raise | n | local_static | keep | - | env cache or stats |
| src/usb/ohci.c | - | s_gone | file_static | reset | usb | ports reported removed to guest |
| src/usb/ohci.c | - | s_hp_settle | file_static | reset | usb | hotplug settle timer |
| src/usb/ohci.c | - | s_det | file_static | reset | usb | deterministic-mode decision |
| src/usb/ohci.c | - | s_det_granted | file_static | reset | usb | deterministic-mode grant |
| src/usb/ohci.c | - | s_det_started | once | reset | usb | rerun: clock origin re-read after clock reset |
| src/usb/ohci.c | - | s_det_ms | file_static | reset | usb | deterministic clock origin |
| src/usb/ohci.c | xbox_OhciInit | done | local_static | keep | - | no rerun: thread and trap pages stay; hook resets |

### src/usb/usb_gamepad.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/usb/usb_gamepad.c | - | s_address | file_static | reset | usb | USB address set by guest; usb_gamepad_detach |
| src/usb/usb_gamepad.c | - | s_configuration | file_static | reset | usb | configuration set by guest; usb_gamepad_detach |
| src/usb/usb_gamepad.c | - | s_source | file_static | keep | - | host pad source callback |
| src/usb/usb_gamepad.c | synthetic_buttons | configured | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | synthetic_buttons | mask | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | synthetic_buttons | period_ms | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | synthetic_buttons | hold_ms | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | synthetic_buttons | t0 | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | - | s_script | file_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | - | s_script_len | file_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | - | s_anchors | file_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | - | s_anchor_pads | file_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | - | s_anchor_lock | file_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | - | s_script_state | file_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | - | s_script_gen | file_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | - | s_flip | file_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | - | s_polls | file_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | - | s_script_poll | file_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | rec_poll | v | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | pad_live_poll | path | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | pad_live_poll | checked | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | pad_live_poll | consumed | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | pad_live_poll | last_poll | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | pad_live_poll | pending | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | pad_live_poll | npending | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | pad_live_poll | next_free | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | pad_script_quit | on | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | pad_script_quit | from | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | pad_script_apply | t0 | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | pad_script_apply | warned | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | pad_script_apply | started | once | keep | - | no rerun: pad script clock runs once |
| src/usb/usb_gamepad.c | script_pads | named | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | script_pads | scanned | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | script_pads | gen | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | - | s_rec_file | file_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | - | s_rec | file_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | - | s_rec_old_int | file_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | - | s_rec_old_term | file_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | - | s_rec_anchor_at | file_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | - | s_rec_anchor_name | file_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | pad_record | state | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | - | s_poll_hook | file_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | usb_gamepad_report | diag | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | usb_gamepad_report | last | local_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | - | s_game_flip | file_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | - | s_game_pressed | file_static | keep | - | pad automation or host diag |
| src/usb/usb_gamepad.c | usb_gamepad_file_open | on | local_static | keep | - | pad automation or host diag |

### src/video/fb_present.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/video/fb_present.c | - | s_fb_running | file_static | keep | - | host window or present stats |
| src/video/fb_present.c | - | s_fb_va | file_static | reset | video | guest framebuffer address |
| src/video/fb_present.c | - | s_fb_pitch | file_static | reset | video | guest framebuffer geometry |
| src/video/fb_present.c | - | s_fb_width | file_static | reset | video | guest framebuffer geometry |
| src/video/fb_present.c | - | s_fb_height | file_static | reset | video | guest framebuffer geometry |
| src/video/fb_present.c | - | s_rgb | file_static | keep | - | host window or present stats |
| src/video/fb_present.c | - | s_present | file_static | keep | - | last frame shown until new flip |
| src/video/fb_present.c | - | s_present_idx | file_static | keep | - | host window or present stats |
| src/video/fb_present.c | - | s_pvideo_source | file_static | reset | video | guest PVIDEO overlay snapshot |
| src/video/fb_present.c | - | s_pvideo_bytes | file_static | reset | video | guest PVIDEO overlay snapshot |
| src/video/fb_present.c | - | s_pvideo_bank | file_static | reset | video | guest PVIDEO overlay snapshot |
| src/video/fb_present.c | - | s_pvideo_regs | file_static | reset | video | guest PVIDEO register snapshot |
| src/video/fb_present.c | - | s_fb_aa_sy | file_static | reset | video | guest AA scale of framebuffer |
| src/video/fb_present.c | - | s_xbe_title | file_static | keep | - | host window or present stats |
| src/video/fb_present.c | - | s_flips | file_static | keep | - | host window or present stats |
| src/video/fb_present.c | - | s_frame_draws | file_static | keep | - | host window or present stats |
| src/video/fb_present.c | - | s_name_gen | file_static | keep | - | host window or present stats |
| src/video/fb_present.c | - | s_host_title_set | file_static | keep | - | host window or present stats |
| src/video/fb_present.c | fb_toggle_fullscreen | full | local_static | keep | - | host window or present stats |
| src/video/fb_present.c | fb_toggle_fullscreen | saved | local_static | keep | - | host window or present stats |
| src/video/fb_present.c | fb_toggle_fullscreen | saved_style | local_static | keep | - | host window or present stats |
| src/video/fb_present.c | - | s_title | file_static | keep | - | host window or present stats |
| src/video/fb_present.c | xbox_FramebufferWindowTitleText | stats | local_static | keep | - | host window or present stats |
| src/video/fb_present.c | gdi_blit | lw | local_static | keep | - | host window or present stats |
| src/video/fb_present.c | gdi_blit | lh | local_static | keep | - | host window or present stats |
| src/video/fb_present.c | fb_thread | frames | local_static | keep | - | host window or present stats |
| src/video/fb_present.c | fb_thread | tc | local_static | keep | - | host window or present stats |
| src/video/fb_present.c | xbox_FramebufferWindowStart | said | local_static | keep | - | host window or present stats |

### src/video/fb_present_metal.m

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/video/fb_present_metal.m | - | s_dev | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_queue | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_pso | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_pso_crop | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_smp | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_view | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_layer | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_unified | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_slot | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_slot_cb | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_conv | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_conv_cap | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_internal | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_layer_filter | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_pso_lines | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_pso_ovl | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_ovl_tex | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_ovl_tex_gen | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_ovl_set | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_ovl_cur | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_chain | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_chain_gen | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_chain_frames | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_chain_ff | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_chain_sp | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_chain_pgen | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_chain_ov_said | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_chain_lines | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_chain_said | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_chain_src | file_static | keep | - | Metal present chain and overlay |
| src/video/fb_present_metal.m | - | s_chain_out | file_static | keep | - | Metal present chain and overlay |

### src/video/fb_present_sdl.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/video/fb_present_sdl.c | - | s_slot | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_back | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_ready | once | keep | - | no rerun: triple-buffer slot index, not init guard |
| src/video/fb_present_sdl.c | - | s_front | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_ready_seq | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_lock | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_running | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_frame_event | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_wake_pending | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_in_present | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_layer | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_flips | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_copy_ns | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_tex_t0 | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_guest_main | file_static | keep | - | entry function pointer |
| src/video/fb_present_sdl.c | - | s_guest_rc | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_guest_done_event | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_title | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_xbe_title | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_host_title_set | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_have_draws | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_frame_draws | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_name_gen | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_shot | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_shot_n | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_ovl_tex | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_ovl_tex_gen | file_static | keep | - | host present slots and window |
| src/video/fb_present_sdl.c | - | s_ovl_conv | file_static | keep | - | host present slots and window |

### src/video/gpu_present.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/video/gpu_present.c | - | s_dev | file_static | keep | - | D3D11 window device and present chain |
| src/video/gpu_present.c | - | s_ctx | file_static | keep | - | D3D11 window device and present chain |
| src/video/gpu_present.c | - | s_chain | file_static | keep | - | D3D11 window device and present chain |
| src/video/gpu_present.c | - | s_rtv | file_static | keep | - | D3D11 window device and present chain |
| src/video/gpu_present.c | - | s_frame | file_static | keep | - | D3D11 window device and present chain |
| src/video/gpu_present.c | - | s_srv | file_static | keep | - | D3D11 window device and present chain |
| src/video/gpu_present.c | - | s_samp | file_static | keep | - | D3D11 window device and present chain |
| src/video/gpu_present.c | - | s_vs | file_static | keep | - | D3D11 window device and present chain |
| src/video/gpu_present.c | - | s_ps | file_static | keep | - | D3D11 window device and present chain |
| src/video/gpu_present.c | - | s_hwnd | file_static | keep | - | D3D11 window device and present chain |
| src/video/gpu_present.c | - | s_w | file_static | keep | - | D3D11 window device and present chain |
| src/video/gpu_present.c | - | s_h | file_static | keep | - | D3D11 window device and present chain |
| src/video/gpu_present.c | - | s_bw | file_static | keep | - | D3D11 window device and present chain |
| src/video/gpu_present.c | - | s_bh | file_static | keep | - | D3D11 window device and present chain |
| src/video/gpu_present.c | - | s_vsync | file_static | keep | - | D3D11 window device and present chain |
| src/video/gpu_present.c | gpu_present_enabled | on | local_static | keep | - | D3D11 window device and present chain |

### src/video/video_player.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/video/video_player.c | - | g_vp | file_static | reset | video | FMV player state |

### src/video/video_pump.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/video/video_pump.c | - | s_playing | file_static | reset | video | FMV pump state |
| src/video/video_pump.c | - | s_path | file_static | reset | video | FMV pump state |

## Enhancements, overlay and mods layer

### src/enhance/enhance_actions.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/enhance/enhance_actions.c | - | s_step_lock | file_static | keep | - | host spin lock over bindings table and port state |
| src/enhance/enhance_actions.c | - | s_reg_lock | file_static | keep | - | host spin lock over the action registry |
| src/enhance/enhance_actions.c | - | s_act | file_static | keep | - | action registry: names, defaults, menu overrides |
| src/enhance/enhance_actions.c | - | s_nact | file_static | keep | - | registered action count |
| src/enhance/enhance_actions.c | - | s_ntoolkit | file_static | keep | - | count of toolkit-owned actions |
| src/enhance/enhance_actions.c | - | s_started | once | keep | - | no rerun: actions start arms keys, host-side only |
| src/enhance/enhance_actions.c | - | s_table | file_static | keep | - | built bindings table, host pad sources |
| src/enhance/enhance_actions.c | - | s_port | file_static | keep | - | host pad held/consumed bits per port |
| src/enhance/enhance_actions.c | - | s_delay_ms | file_static | keep | - | input.chord_delay_ms from config |
| src/enhance/enhance_actions.c | - | s_armed | file_static | keep | - | pad filter and flip hook installed flag |
| src/enhance/enhance_actions.c | - | s_arm_lock | file_static | keep | - | host spin lock over the armed key list |
| src/enhance/enhance_actions.c | - | s_arm_keys | file_static | keep | - | feature keys that arm the hook |
| src/enhance/enhance_actions.c | - | s_arm_owner | file_static | keep | - | mod owning each armed key |
| src/enhance/enhance_actions.c | - | s_narm | file_static | keep | - | armed key count |
| src/enhance/enhance_actions.c | - | s_log_sec | file_static | keep | - | fire log rate limit second |
| src/enhance/enhance_actions.c | - | s_log_n | file_static | keep | - | fire log lines this second |
| src/enhance/enhance_actions.c | - | s_log_dropped | file_static | keep | - | fire log lines dropped |
| src/enhance/enhance_actions.c | - | s_stats_act | file_static | keep | - | overlay.stats action id |
| src/enhance/enhance_actions.c | - | s_menu_act | file_static | keep | - | menu.open action id |
| src/enhance/enhance_actions.c | - | s_menu_hook | file_static | keep | - | menu's post-step hook fn pointer |
| src/enhance/enhance_actions.c | - | s_panel_hook | file_static | keep | - | menu panel stop hook fn pointer |
| src/enhance/enhance_actions.c | - | s_menu_req | file_static | keep | - | pending menu.open request; menu UI state |
| src/enhance/enhance_actions.c | - | s_quiet | file_static | keep | - | menu capture quiets action fires |
| src/enhance/enhance_actions.c | - | s_why | file_static | keep | - | per-thread reason buffer for enhance_action_check |
| src/enhance/enhance_actions.c | - | s_why_n | file_static | keep | - | size of the per-thread reason buffer |

### src/enhance/enhance_cfg.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/enhance/enhance_cfg.c | - | s_binds | file_static | keep | - | env-to-config key bindings |
| src/enhance/enhance_cfg.c | - | s_n_binds | file_static | keep | - | binding count |
| src/enhance/enhance_cfg.c | - | s_builtin_bound | file_static | keep | - | built-in bindings registered flag |
| src/enhance/enhance_cfg.c | - | s_code_defs | file_static | keep | - | a game's code defaults (enhance_cfg_set_code_default), set once before init |
| src/enhance/enhance_cfg.c | - | s_n_code_defs | file_static | keep | - | code default count |
| src/enhance/enhance_cfg.c | - | s_file | file_static | keep | - | parsed config files (defaults, user) |
| src/enhance/enhance_cfg.c | - | s_flags | file_static | keep | - | per-entry read/warned bits for typo report |
| src/enhance/enhance_cfg.c | - | s_path | file_static | keep | - | each slot's file path, for the refresh after a write (enhance_cfg_patch) |
| src/enhance/enhance_cfg.c | - | s_retired | file_static | keep | - | copies a refresh replaced, freed at enhance_cfg_shutdown |
| src/enhance/enhance_cfg.c | - | s_n_retired | file_static | keep | - | retired copy count |
| src/enhance/enhance_cfg.c | - | s_cap_retired | file_static | keep | - | retired list capacity |
| src/enhance/enhance_cfg.c | - | s_user_path | file_static | keep | - | user config path the menu Save writes |
| src/enhance/enhance_cfg.c | - | s_user_dir | file_static | keep | - | user config directory |
| src/enhance/enhance_cfg.c | - | s_owner | file_static | keep | - | mod ids for naming stray key owners |
| src/enhance/enhance_cfg.c | - | s_nowner | file_static | keep | - | mod id count |

### src/enhance/enhance_input.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/enhance/enhance_input.c | - | s_set | file_static | keep | - | parsed [input.controls] per file |
| src/enhance/enhance_input.c | - | s_pad | file_static | keep | - | parsed [input.pad] per file |
| src/enhance/enhance_input.c | - | s_mod_capture | file_static | reset | mods | a mod's pad/mouse capture; free camera's, released at reset |
| src/enhance/enhance_input.c | - | s_menu_hold | file_static | keep | - | menu holds the mouse; UI state |
| src/enhance/enhance_input.c | - | s_div_lock | file_static | keep | - | host spin lock over capture and divert |

### src/enhance/enhance_menu.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/enhance/enhance_menu.c | - | s_page_intro | file_static | keep | - | page intro texts |
| src/enhance/enhance_menu.c | - | s_pack_off | file_static | keep | - | menu's texture-pack off bits |
| src/enhance/enhance_menu.c | - | s_language | file_static | keep | - | language menu item |
| src/enhance/enhance_menu.c | - | s_language_help | file_static | keep | - | language row help text |
| src/enhance/enhance_menu.c | - | s_slot | file_static | keep | - | built menu slots |
| src/enhance/enhance_menu.c | - | s_mod_toggle | file_static | keep | - | mod toggle items |
| src/enhance/enhance_menu.c | - | s_mod_key | file_static | keep | - | mod toggle config keys |
| src/enhance/enhance_menu.c | - | s_nslot | file_static | keep | - | slot count |
| src/enhance/enhance_menu.c | - | s_built | file_static | keep | - | slots built flag |
| src/enhance/enhance_menu.c | - | s_title | file_static | keep | - | game's registered menu items |
| src/enhance/enhance_menu.c | - | s_ntitle | file_static | keep | - | game item count |
| src/enhance/enhance_menu.c | - | s_set_item | file_static | keep | - | descriptor setting items |
| src/enhance/enhance_menu.c | - | s_nset | file_static | keep | - | setting item count |
| src/enhance/enhance_menu.c | - | s_abase | file_static | keep | - | bindings as loaded, for unsaved state |
| src/enhance/enhance_menu.c | - | s_abase_n | file_static | keep | - | loaded binding counts |
| src/enhance/enhance_menu.c | - | s_abase_have | file_static | keep | - | loaded bindings valid flags |
| src/enhance/enhance_menu.c | - | s_state | file_static | keep | - | menu open/closed state |
| src/enhance/enhance_menu.c | - | s_page | file_static | keep | - | current page |
| src/enhance/enhance_menu.c | - | s_sel | file_static | keep | - | per-page selection |
| src/enhance/enhance_menu.c | - | s_shaders_off | file_static | keep | - | shaders page disabled flag |
| src/enhance/enhance_menu.c | - | s_shaders_said | file_static | keep | - | shaders-off said once |
| src/enhance/enhance_menu.c | - | s_scroll | file_static | keep | - | per-page scroll |
| src/enhance/enhance_menu.c | - | s_mod_sub | file_static | keep | - | mod listed on Mods page |
| src/enhance/enhance_menu.c | - | s_confirm | file_static | keep | - | settings Reset row confirm |
| src/enhance/enhance_menu.c | - | s_dlg | file_static | keep | - | open dialog state |
| src/enhance/enhance_menu.c | - | s_hints | file_static | keep | - | overlay.menu_hints |
| src/enhance/enhance_menu.c | - | s_size | file_static | keep | - | overlay.menu_size |
| src/enhance/enhance_menu.c | - | s_focus | file_static | keep | - | menu focus area |
| src/enhance/enhance_menu.c | - | s_foot_sel | file_static | keep | - | footer selection |
| src/enhance/enhance_menu.c | - | s_peek_t | file_static | keep | - | section peek time |
| src/enhance/enhance_menu.c | - | s_peek_drawn | file_static | keep | - | peek redraw done |
| src/enhance/enhance_menu.c | - | s_need_draw | file_static | keep | - | redraw request |
| src/enhance/enhance_menu.c | - | s_status | file_static | keep | - | menu status line text |
| src/enhance/enhance_menu.c | - | s_status_t | file_static | keep | - | status line time |
| src/enhance/enhance_menu.c | - | s_now | file_static | keep | - | menu clock, host ms |
| src/enhance/enhance_menu.c | - | s_row | file_static | keep | - | page rows as drawn |
| src/enhance/enhance_menu.c | - | s_nrow | file_static | keep | - | row count |
| src/enhance/enhance_menu.c | - | s_cap | file_static | keep | - | binding capture state |
| src/enhance/enhance_menu.c | - | s_held | file_static | keep | - | host pad buttons held per port |
| src/enhance/enhance_menu.c | - | s_prev_held | file_static | keep | - | previous held buttons |
| src/enhance/enhance_menu.c | - | s_rel_armed | file_static | keep | - | wait-for-release bits |
| src/enhance/enhance_menu.c | - | s_rel_taint | file_static | keep | - | release taint bits |
| src/enhance/enhance_menu.c | - | s_kprev | file_static | keep | - | previous host key states |
| src/enhance/enhance_menu.c | - | s_pkt | file_static | keep | - | host pad packet numbers |
| src/enhance/enhance_menu.c | - | s_fpkt | file_static | keep | - | host pad packet numbers, filter side |
| src/enhance/enhance_menu.c | - | s_dir | file_static | keep | - | menu nav direction held |
| src/enhance/enhance_menu.c | - | s_kbd | file_static | keep | - | keyboard leads flag |
| src/enhance/enhance_menu.c | - | s_mouse | file_static | keep | - | mouse leads flag |
| src/enhance/enhance_menu.c | - | s_kbd_seen | file_static | keep | - | physical keyboard seen |
| src/enhance/enhance_menu.c | - | s_ptr_seq | file_static | keep | - | pointer snapshot sequence |
| src/enhance/enhance_menu.c | - | s_ptr_ax | file_static | keep | - | pointer motion anchor x |
| src/enhance/enhance_menu.c | - | s_ptr_ay | file_static | keep | - | pointer motion anchor y |
| src/enhance/enhance_menu.c | - | s_ptr_x | file_static | keep | - | pointer x |
| src/enhance/enhance_menu.c | - | s_ptr_y | file_static | keep | - | pointer y |
| src/enhance/enhance_menu.c | - | s_ptr_btn | file_static | keep | - | pointer buttons held |
| src/enhance/enhance_menu.c | - | s_drag_row | file_static | keep | - | slider row being dragged |
| src/enhance/enhance_menu.c | - | s_free_scroll | file_static | keep | - | wheel free scroll |
| src/enhance/enhance_menu.c | - | s_hover_row | file_static | keep | - | hovered row |
| src/enhance/enhance_menu.c | - | s_tip_t | file_static | keep | - | tooltip settle time |
| src/enhance/enhance_menu.c | - | s_tip_sel | file_static | keep | - | tooltip selection |
| src/enhance/enhance_menu.c | - | s_tip_page | file_static | keep | - | tooltip page |
| src/enhance/enhance_menu.c | - | s_lay | file_static | keep | - | last draw's layout |
| src/enhance/enhance_menu.c | - | s_lay_ok | file_static | keep | - | layout valid |
| src/enhance/enhance_menu.c | - | s_tip_drawn | file_static | keep | - | tooltip drawn flag |
| src/enhance/enhance_menu.c | - | s_dir_next | file_static | keep | - | nav repeat time |
| src/enhance/enhance_menu.c | - | s_close_t | file_static | keep | - | menu close time |
| src/enhance/enhance_menu.c | - | s_draw_t | file_static | keep | - | last draw time |
| src/enhance/enhance_menu.c | - | s_extra_t | file_static | keep | - | per-source repeat times |
| src/enhance/enhance_menu.c | - | s_snap_on | file_static | keep | - | snapshot toggle on |
| src/enhance/enhance_menu.c | - | s_stats_on | file_static | keep | - | stats toggle on |
| src/enhance/enhance_menu.c | - | s_draw_w | file_static | keep | - | last draw width |
| src/enhance/enhance_menu.c | - | s_draw_h | file_static | keep | - | last draw height |
| src/enhance/enhance_menu.c | - | s_draw_atlas | file_static | keep | - | atlas generation last draw used |
| src/enhance/enhance_menu.c | - | s_rate_t0 | file_static | keep | - | stats rate window start |
| src/enhance/enhance_menu.c | - | s_rate_c0 | file_static | keep | - | nv2a stat totals at window start |
| src/enhance/enhance_menu.c | - | s_rate | file_static | keep | - | stats per-second rates |
| src/enhance/enhance_menu.c | text_rows | l | local_static | keep | - | scratch layout off the stack |
| src/enhance/enhance_menu.c | - | s_ptab | file_static | keep | - | present shader params |
| src/enhance/enhance_menu.c | - | s_pitem | file_static | keep | - | present param menu items |
| src/enhance/enhance_menu.c | - | s_nptab | file_static | keep | - | present param count |
| src/enhance/enhance_menu.c | - | s_pkey_active | file_static | keep | - | active present config key |
| src/enhance/enhance_menu.c | - | s_sh_sub | file_static | keep | - | shaders sub-page |
| src/enhance/enhance_menu.c | - | s_sh_conf | file_static | keep | - | shaders confirm |
| src/enhance/enhance_menu.c | - | s_sh_conf_type | file_static | keep | - | shaders confirm type |
| src/enhance/enhance_menu.c | - | s_sh_conf_ref | file_static | keep | - | shaders confirm ref |
| src/enhance/enhance_menu.c | - | s_sh_want | file_static | keep | - | shaders pending choice |
| src/enhance/enhance_menu.c | - | s_sh_want_type | file_static | keep | - | pending choice type |
| src/enhance/enhance_menu.c | - | s_sh_want_ref | file_static | keep | - | pending choice ref |
| src/enhance/enhance_menu.c | - | s_pick | file_static | keep | - | profile pick list |
| src/enhance/enhance_menu.c | - | s_npick | file_static | keep | - | pick count |
| src/enhance/enhance_menu.c | - | s_pnote | file_static | keep | - | param notes |
| src/enhance/enhance_menu.c | - | s_pkind | file_static | keep | - | param kind info |
| src/enhance/enhance_menu.c | - | s_pchoice | file_static | keep | - | param choice state |
| src/enhance/enhance_menu.c | - | s_name_on | file_static | keep | - | rename field open |
| src/enhance/enhance_menu.c | - | s_name_for | file_static | keep | - | rename target |
| src/enhance/enhance_menu.c | - | s_name | file_static | keep | - | rename text |
| src/enhance/enhance_menu.c | - | s_name_cur | file_static | keep | - | rename cursor |
| src/enhance/enhance_menu.c | - | s_name_dir | file_static | keep | - | rename repeat dir |
| src/enhance/enhance_menu.c | - | s_name_next | file_static | keep | - | rename repeat time |
| src/enhance/enhance_menu.c | - | s_osk | file_static | keep | - | on-screen keyboard state |
| src/enhance/enhance_menu.c | - | s_osk_lay | file_static | keep | - | OSK layout |
| src/enhance/enhance_menu.c | - | s_osk_lay_ok | file_static | keep | - | OSK layout valid |
| src/enhance/enhance_menu.c | - | s_fb_page | file_static | keep | - | page the file browser serves |
| src/enhance/enhance_menu.c | - | s_pkey | file_static | keep | - | unsaved pending keys |
| src/enhance/enhance_menu.c | - | s_pval | file_static | keep | - | unsaved pending values |
| src/enhance/enhance_menu.c | - | s_pnull | file_static | keep | - | pending key removals |
| src/enhance/enhance_menu.c | - | s_kkey | file_static | keep | - | kept keys on save |
| src/enhance/enhance_menu.c | - | s_kval | file_static | keep | - | kept values on save |
| src/enhance/enhance_menu.c | - | s_nkept | file_static | keep | - | kept key count |
| src/enhance/enhance_menu.c | - | s_rw_done | file_static | keep | - | alias rewrites saved |
| src/enhance/enhance_menu.c | - | s_rw_staged | file_static | keep | - | rewrites staged |
| src/enhance/enhance_menu.c | - | s_rw_from | file_static | keep | - | rewrite range start |
| src/enhance/enhance_menu.c | - | s_rw_to | file_static | keep | - | rewrite range end |
| src/enhance/enhance_menu.c | - | s_fb | file_static | keep | - | file browser state |
| src/enhance/enhance_menu.c | - | s_fb_ready | once | keep | - | no rerun: file browser init is host-side |
| src/enhance/enhance_menu.c | - | s_fb_view | file_static | keep | - | file browser view |
| src/enhance/enhance_menu.c | - | s_fb_job | file_static | keep | - | file browser job |
| src/enhance/enhance_menu.c | - | s_fbx | file_static | keep | - | file export state |
| src/enhance/enhance_menu.c | - | s_imp | file_static | keep | - | config/profile import plan |
| src/enhance/enhance_menu.c | - | s_saves | file_static | keep | - | HDD saves list, read lazily |
| src/enhance/enhance_menu.c | - | s_saves_read | file_static | keep | - | saves list read state |
| src/enhance/enhance_menu.c | - | s_saves_quit | file_static | keep | - | saves imported, quit pending |
| src/enhance/enhance_menu.c | - | s_simp | file_static | keep | - | saves import plan |
| src/enhance/enhance_menu.c | files_import_plan | p | local_static | keep | - | scratch import profile |
| src/enhance/enhance_menu.c | - | s_lay_pages | file_static | keep | - | per-page layout cache |
| src/enhance/enhance_menu.c | enhance_menu_draw | kinds | local_static | keep | - | scratch row kinds |
| src/enhance/enhance_menu.c | enhance_menu_draw | vw | local_static | keep | - | scratch row widths |

### src/enhance/enhance_mod_status.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/enhance/enhance_mod_status.c | - | s_device | file_static | keep | - | last host input device used |
| src/enhance/enhance_mod_status.c | - | s_src_down | file_static | keep | - | host pad sources held, for glyph device |
| src/enhance/enhance_mod_status.c | - | s_stick_out | file_static | keep | - | host stick deflection flags |
| src/enhance/enhance_mod_status.c | - | s_vk_down | file_static | keep | - | host keys held |
| src/enhance/enhance_mod_status.c | - | s_serial | file_static | keep | - | status text serial last composed |
| src/enhance/enhance_mod_status.c | - | s_shown_device | file_static | keep | - | device of last panel compose |
| src/enhance/enhance_mod_status.c | - | s_shown_family | file_static | keep | - | pad glyph family of last compose |
| src/enhance/enhance_mod_status.c | - | s_shown_w | file_static | keep | - | panel width last composed |
| src/enhance/enhance_mod_status.c | - | s_shown_h | file_static | keep | - | panel height last composed |
| src/enhance/enhance_mod_status.c | - | s_shown_atlas | file_static | keep | - | glyph atlas generation last composed |
| src/enhance/enhance_mod_status.c | - | s_shown_ms | file_static | keep | - | time panel last shown |
| src/enhance/enhance_mod_status.c | - | s_shown | file_static | keep | - | panel shown flag |
| src/enhance/enhance_mod_status.c | - | s_stop_gen | file_static | keep | - | stop generation for the flip tick |
| src/enhance/enhance_mod_status.c | - | s_seen_gen | file_static | keep | - | stop generation the tick saw |

### src/enhance/enhance_osk.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/enhance/enhance_osk.c | enhance_osk_key | one | local_static | constant | - | lazily built one-char key labels from fixed rows |

### src/enhance/enhance_text.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/enhance/enhance_text.c | key_namer | s_hkl | local_static | keep | - | host keyboard layout the names were built for |
| src/enhance/enhance_text.c | key_namer | s_name | local_static | keep | - | host key name cache, keyed by layout |
| src/enhance/enhance_text.c | key_namer | s_have | local_static | keep | - | host key name cache valid bits |

### src/enhance/hud_anchor.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/enhance/hud_anchor.c | - | s_cv | file_static | constant | - | HUD table canvas parse; fixed XBE VAs |
| src/enhance/hud_anchor.c | - | s_stride | file_static | constant | - | HUD table vertex stride |
| src/enhance/hud_anchor.c | - | s_x_off | file_static | constant | - | HUD table vertex x offset |
| src/enhance/hud_anchor.c | - | s_prim | file_static | constant | - | HUD table primitive type |
| src/enhance/hud_anchor.c | - | s_u_off | file_static | constant | - | HUD table vertex u offset |
| src/enhance/hud_anchor.c | - | s_groups | file_static | constant | - | HUD groups parse; refs epoch-stamped, stale ones ignored |
| src/enhance/hud_anchor.c | - | s_ngroups | file_static | constant | - | HUD group count |
| src/enhance/hud_anchor.c | - | s_sites | file_static | constant | - | HUD sites by fixed return VA |
| src/enhance/hud_anchor.c | - | s_nsites | file_static | constant | - | HUD site count |
| src/enhance/hud_anchor.c | - | s_nwrappers | file_static | constant | - | HUD wrapper count |
| src/enhance/hud_anchor.c | - | s_table | file_static | keep | - | a HUD table is registered; mod registration stays |
| src/enhance/hud_anchor.c | - | s_on | file_static | reset | enhance | HUD active flag from the guest canvas |
| src/enhance/hud_anchor.c | - | s_mod | file_static | reset | enhance | HUD mod part active, from guest canvas pad |
| src/enhance/hud_anchor.c | - | s_cap | file_static | keep | - | a HUD mapping capture runs (debug) |
| src/enhance/hud_anchor.c | - | s_table_cap | file_static | keep | - | table came from hud_capture_table (debug) |
| src/enhance/hud_anchor.c | - | s_rec | file_static | keep | - | capture recorder fn pointer |
| src/enhance/hud_anchor.c | - | s_draws | file_static | constant | - | capture.draws parse, fixed VAs |
| src/enhance/hud_anchor.c | - | s_ndraws | file_static | constant | - | capture draw count |
| src/enhance/hud_anchor.c | - | s_memp | file_static | keep | - | host address of api.mem; guest memory stays mapped |
| src/enhance/hud_anchor.c | - | s_epoch | file_static | keep | - | monotonic frame epoch; stale refs and chains self-expire |
| src/enhance/hud_anchor.c | - | s_aw | file_static | keep | - | display.hud_aspect width |
| src/enhance/hud_anchor.c | - | s_ah | file_static | keep | - | display.hud_aspect height |
| src/enhance/hud_anchor.c | - | s_margin | file_static | keep | - | display.hud_margin |
| src/enhance/hud_anchor.c | - | s_snap | file_static | reset | enhance | guest frame's canvas and target snapshot |
| src/enhance/hud_anchor.c | - | s_snap_seq | file_static | keep | - | seqlock sequence; must stay even across reset |
| src/enhance/hud_anchor.c | - | s_shown | file_static | keep | - | trace change-line state |
| src/enhance/hud_anchor.c | - | s_shown_why | file_static | keep | - | trace change-line reason |
| src/enhance/hud_anchor.c | - | s_shown_valid | file_static | keep | - | trace change-line valid flag |
| src/enhance/hud_anchor.c | - | s_lift_shown | file_static | keep | - | trace lift line state |
| src/enhance/hud_anchor.c | - | s_lift_pad | file_static | keep | - | trace lift line pad |
| src/enhance/hud_anchor.c | - | s_lift_hud | file_static | keep | - | trace lift line hud pad |
| src/enhance/hud_anchor.c | - | t_chain | file_static | reset | enhance | per-thread wrapper chain of guest ret/esp |
| src/enhance/hud_anchor.c | - | t_nchain | file_static | reset | enhance | per-thread wrapper chain depth |
| src/enhance/hud_anchor.c | - | t_save | file_static | reset | enhance | per-thread RESTORE saves: guest esp, VA, old x/u |
| src/enhance/hud_anchor.c | - | t_nsave | file_static | reset | enhance | per-thread RESTORE save depth |
| src/enhance/hud_anchor.c | trace_on | on | local_static | keep | - | cached RENV_HUD_TRACE |
| src/enhance/hud_anchor.c | - | s_seen | file_static | keep | - | trace say-once set |

### src/enhance/hud_capture.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/enhance/hud_capture.c | - | s_on | file_static | keep | - | debug.hud_capture on |
| src/enhance/hud_capture.c | - | s_dir | file_static | keep | - | capture output directory |
| src/enhance/hud_capture.c | - | s_from | file_static | keep | - | debug trigger: first frame captured |
| src/enhance/hud_capture.c | - | s_count | file_static | keep | - | debug trigger: frames to capture |
| src/enhance/hud_capture.c | - | s_memp | file_static | keep | - | host address of api.mem; guest memory stays mapped |
| src/enhance/hud_capture.c | - | s_out | file_static | keep | - | capture output file |
| src/enhance/hud_capture.c | - | s_finished | file_static | keep | - | capture finished flag |
| src/enhance/hud_capture.c | - | s_flock | file_static | keep | - | host spin lock frame vs shutdown |
| src/enhance/hud_capture.c | - | s_warned | file_static | keep | - | one-time warning bits |
| src/enhance/hud_capture.c | - | s_tick | file_static | keep | - | debug trigger progress: mod frames seen |
| src/enhance/hud_capture.c | - | s_rec | file_static | keep | - | debug: frame being recorded |
| src/enhance/hud_capture.c | - | s_lock | file_static | keep | - | host spin lock over line buffer |
| src/enhance/hud_capture.c | - | s_buf | file_static | keep | - | capture line buffer |
| src/enhance/hud_capture.c | - | s_len | file_static | keep | - | capture line buffer length |
| src/enhance/hud_capture.c | - | s_cap | file_static | keep | - | capture line buffer capacity |
| src/enhance/hud_capture.c | - | s_dropped | file_static | keep | - | capture lines dropped count |
| src/enhance/hud_capture.c | - | s_sprites | file_static | keep | - | capture sprite count |
| src/enhance/hud_capture.c | - | s_pend | file_static | keep | - | frames awaiting their picture, written in order |
| src/enhance/hud_capture.c | - | s_npend | file_static | keep | - | pending frame count |
| src/enhance/hud_capture.c | - | s_frames_out | file_static | keep | - | capture frames written count |
| src/enhance/hud_capture.c | - | s_bmps_out | file_static | keep | - | capture BMPs written count |

### src/enhance/mod.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/enhance/mod.c | - | s_table | file_static | keep | - | the game's mod table |
| src/enhance/mod.c | - | s_running | file_static | keep | - | mods whose init ran |
| src/enhance/mod.c | - | s_nrunning | file_static | keep | - | running mod count |
| src/enhance/mod.c | - | s_init_owner | file_static | keep | - | mod whose init runs; on_reset needs it set |
| src/enhance/mod.c | - | s_frame | file_static | keep | - | mods' on_frame callbacks, host fn pointers |
| src/enhance/mod.c | - | s_nframe | file_static | keep | - | on_frame count |
| src/enhance/mod.c | - | s_filter | file_static | keep | - | mods' batch filters, host fn pointers |
| src/enhance/mod.c | - | s_nfilter | file_static | keep | - | batch filter count |
| src/enhance/mod.c | - | s_items | file_static | keep | - | mods' menu items |
| src/enhance/mod.c | - | s_nitems | file_static | keep | - | mod menu item count |
| src/enhance/mod.c | - | s_count | file_static | keep | - | mod table entries |
| src/enhance/mod.c | - | s_state | file_static | keep | - | mod on/off/restart state; mod settings |
| src/enhance/mod.c | - | s_started | once | keep | - | no rerun: mod inits stay, on_reset replaces them (D6a) |
| src/enhance/mod.c | - | s_want | file_static | keep | - | menu's wanted mod state |
| src/enhance/mod.c | - | s_pending | file_static | keep | - | menu toggle pending per mod |
| src/enhance/mod.c | - | s_any_pending | file_static | keep | - | a toggle waits flag |
| src/enhance/mod.c | - | s_on_enable | file_static | keep | - | mods' on_enable callbacks |
| src/enhance/mod.c | - | s_on_enable_user | file_static | keep | - | on_enable user pointers, host |
| src/enhance/mod.c | - | s_key | file_static | keep | - | mods.<name> config keys |
| src/enhance/mod.c | - | s_mod | file_static | keep | - | descriptor runtime: settings, controls, bindings |
| src/enhance/mod.c | - | s_menu_on | file_static | keep | - | menu.enabled at init |
| src/enhance/mod.c | - | s_rw | file_static | keep | - | pending config alias rewrites |
| src/enhance/mod.c | - | s_nrw | file_static | keep | - | pending rewrite count |
| src/enhance/mod.c | - | s_shut | file_static | keep | - | shutdown ran flag |
| src/enhance/mod.c | - | s_exit_hooked | file_static | keep | - | exit hook installed flag |
| src/enhance/mod.c | - | s_api | file_static | keep | - | mod API table; mem is host base, unchanged |
| src/enhance/mod.c | - | s_active | file_static | reset | mods | each mod's last api->active from the old boot |
| src/enhance/mod.c | - | s_hud_owner | file_static | keep | - | mod that registered the HUD table |
| src/enhance/mod.c | - | s_hud_owner_on | file_static | reset | mods | HUD owner state latched at the last guest frame |
| src/enhance/mod.c | - | s_set_at | file_static | keep | - | RECOMP_DEBUG mod_set_at entries (debug) |
| src/enhance/mod.c | - | s_nset_at | file_static | keep | - | mod_set_at entry count (debug) |
| src/enhance/mod.c | - | s_frames | file_static | keep | - | debug trigger progress: guest frames for mod_set_at |
| src/enhance/mod.c | - | s_follows_on | file_static | keep | - | follows key differs from default, read at init |
| src/enhance/mod.c | - | s_token | file_static | keep | - | mods= token last printed |
| src/enhance/mod.c | - | s_hooks | file_static | reset | mods | mods' guest hook records (pre, post, user, owner) |
| src/enhance/mod.c | - | s_nhooks | file_static | reset | mods | mod guest hook count |
| src/enhance/mod.c | - | s_cap_hooks | file_static | keep | - | HUD capture's own hooks (debug) |
| src/enhance/mod.c | - | s_ncap_hooks | file_static | keep | - | HUD capture hook count (debug) |
| src/enhance/mod.c | - | s_cap_owner | file_static | keep | - | owner token address for capture hooks |
| src/enhance/mod.c | - | t_in_guest_cb | tls | reset | mods | per-thread in-guest-callback depth; unwind leaves it raised |
| src/enhance/mod.c | - | s_galloc_va | file_static | reset | mods | each mod's guest_alloc VA in the cleared arena |
| src/enhance/mod.c | - | s_galloc_size | file_static | reset | mods | each mod's guest_alloc block size |
| src/enhance/mod.c | - | s_galloc_lock | file_static | keep | - | host spin lock over guest_alloc |
| src/enhance/mod.c | - | s_status | file_static | reset | mods | mods' status panel text, often guest values |
| src/enhance/mod.c | - | s_status_lock | file_static | keep | - | host spin lock over status text |
| src/enhance/mod.c | - | s_status_serial | file_static | keep | - | status change serial the panel watches |
| src/enhance/mod.c | - | s_glyphs_used | file_static | keep | - | some mod used glyph tokens |
| src/enhance/mod.c | - | s_namer | file_static | keep | - | glyph namer fn pointer |
| src/enhance/mod.c | - | s_dbg_last | file_static | reset | mods | mod whose 3D debug frame shows |
| src/enhance/mod.c | - | s_dbg_open | file_static | reset | mods | mod whose 3D debug frame is open; unwind leaves it |
| src/enhance/mod.c | - | s_rec | file_static | reset | mods | port 0 pad record for a capturing mod |
| src/enhance/mod.c | - | s_rec_busy | file_static | keep | - | record writer busy flag, host poll thread |
| src/enhance/mod.c | - | s_cap_epoch | file_static | keep | - | monotonic capture epoch; bump invalidates s_rec |
| src/enhance/mod.c | - | s_code_lock | file_static | keep | - | host spin lock over controls' parsed sources |

### src/enhance/mod_describe.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/enhance/mod_describe.c | shader_preset | p | local_static | keep | - | scratch present params off the stack |
| src/enhance/mod_describe.c | profiles | l | local_static | keep | - | scratch profile list off the stack |

### src/enhance/saves_store.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/enhance/saves_store.c | - | s_rename_fail | file_static | keep | - | test hook for rename failure |

### src/enhance/saves_zip.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/enhance/saves_zip.c | - | s_sort_base | file_static | keep | - | qsort context pointer, set per sort |

### src/enhance/shader_param_kinds.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/enhance/shader_param_kinds.c | - | s_builtin | file_static | constant | - | built-in param kinds parsed from fixed file |
| src/enhance/shader_param_kinds.c | - | s_builtin_read | file_static | constant | - | built-in kinds read flag |
| src/enhance/shader_param_kinds.c | - | s_builtin_path | file_static | constant | - | built-in kinds file path |
| src/enhance/shader_param_kinds.c | - | s_prof | file_static | keep | - | profile's param kinds, keyed by path and mtime |
| src/enhance/shader_param_kinds.c | - | s_prof_path | file_static | keep | - | profile kinds cache path |
| src/enhance/shader_param_kinds.c | - | s_prof_mtime | file_static | keep | - | profile kinds cache mtime |
| src/enhance/shader_param_kinds.c | - | s_prof_nsec | file_static | keep | - | profile kinds cache mtime nsec |
| src/enhance/shader_param_kinds.c | - | s_prof_size | file_static | keep | - | profile kinds cache size |

### src/enhance/shader_profile_edit.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/enhance/shader_profile_edit.c | - | s_e | file_static | keep | - | shader editor entries |
| src/enhance/shader_profile_edit.c | - | s_tmp | file_static | keep | - | shader editor scratch entries |
| src/enhance/shader_profile_edit.c | - | s_n | file_static | keep | - | editor entry count |
| src/enhance/shader_profile_edit.c | - | s_active | file_static | keep | - | active editor entry |
| src/enhance/shader_profile_edit.c | - | s_init | once | keep | - | no rerun: editor list from host profile files |
| src/enhance/shader_profile_edit.c | - | s_saved_kind | file_static | keep | - | last saved profile kind |
| src/enhance/shader_profile_edit.c | - | s_saved_slug | file_static | keep | - | last saved profile slug |
| src/enhance/shader_profile_edit.c | - | s_deleted | file_static | keep | - | a profile was deleted |
| src/enhance/shader_profile_edit.c | - | s_info | file_static | keep | - | composed profile info |
| src/enhance/shader_profile_edit.c | - | s_info_ok | file_static | keep | - | profile info valid |
| src/enhance/shader_profile_edit.c | - | s_info_err | file_static | keep | - | profile info error text |
| src/enhance/shader_profile_edit.c | - | s_missing | file_static | keep | - | stages whose shader file is missing |
| src/enhance/shader_profile_edit.c | - | s_names | file_static | keep | - | per-stage param names |
| src/enhance/shader_profile_edit.c | - | s_nnames | file_static | keep | - | per-stage param counts |
| src/enhance/shader_profile_edit.c | - | s_list | file_static | keep | - | profile list from disk |
| src/enhance/shader_profile_edit.c | - | s_p | file_static | keep | - | profile being edited |
| src/enhance/shader_profile_edit.c | - | s_fp | file_static | keep | - | profile as on disk |
| src/enhance/shader_profile_edit.c | - | s_rows | file_static | keep | - | present param rows |
| src/enhance/shader_profile_edit.c | - | s_vname | file_static | keep | - | value names being edited |
| src/enhance/shader_profile_edit.c | - | s_vv | file_static | keep | - | values being edited |
| src/enhance/shader_profile_edit.c | - | s_ckey | file_static | keep | - | config keys a save writes |
| src/enhance/shader_profile_edit.c | - | s_cval | file_static | keep | - | config values a save writes |
| src/enhance/shader_profile_edit.c | - | s_cnull | file_static | keep | - | config keys a save removes |
| src/enhance/shader_profile_edit.c | shader_edit_refresh | kept | local_static | keep | - | scratch kept-entry flags |
| src/enhance/shader_profile_edit.c | shader_edit_entry | off | local_static | keep | - | the Off entry |
| src/enhance/shader_profile_edit.c | shader_edit_presets | stem | local_static | keep | - | scratch list of preset stems, refilled per call |
| src/enhance/shader_profile_edit.c | free_slug | taken | local_static | keep | - | scratch list of taken slugs, refilled per call |

### src/enhance/shader_profile_req.c

| file | scope | name | kind | class | hook | why |
|---|---|---|---|---|---|---|
| src/enhance/shader_profile_req.c | - | s_root | file_static | keep | - | shader profile root directory |
| src/enhance/shader_profile_req.c | - | s_req_lock | file_static | keep | - | host flag lock over profile requests |
| src/enhance/shader_profile_req.c | - | s_stage | file_static | keep | - | present chain stages requested |
| src/enhance/shader_profile_req.c | - | s_work | file_static | keep | - | profile request scratch |
| src/enhance/shader_profile_req.c | - | s_pending | file_static | keep | - | start profile waiting for first present |
| src/enhance/shader_profile_req.c | - | s_start_slug | file_static | keep | - | profile the start chose |
| src/enhance/shader_profile_req.c | - | s_start_gen | file_static | keep | - | present chain generation at start |
| src/enhance/shader_profile_req.c | - | s_start_loaded | file_static | keep | - | start profile loaded flag |
| src/enhance/shader_profile_req.c | - | s_start_waiting | file_static | keep | - | start request waits for first present |
| src/enhance/shader_profile_req.c | - | s_stale | file_static | keep | - | stale profile names reported |
| src/enhance/shader_profile_req.c | - | s_nstale | file_static | keep | - | stale name count |
| src/enhance/shader_profile_req.c | request_locked | names | local_static | keep | - | scratch param name pointers |
| src/enhance/shader_profile_req.c | request_locked | rows | local_static | keep | - | scratch present params |
| src/enhance/shader_profile_req.c | shader_profile_place | rows | local_static | keep | - | scratch present params |
| src/enhance/shader_profile_req.c | shader_profile_place | name | local_static | keep | - | scratch param names |
| src/enhance/shader_profile_req.c | shader_profile_place | names | local_static | keep | - | scratch param name pointers |

## Notes

### Kernel (src/kernel, outside nv2a_*)

- kernel_hal.c: `s_dispatch_gate` is a process-wide CS taken at DISPATCH_LEVEL. A thread that unwinds raised leaves it held, and `g_irql_raised_count` stays raised. The hook must abandon it and replace it, not just zero `s_gate_owner`.
- kernel_hal.c: `s_irq_run` and the post/release hooks are host fn ptrs set by `xbox_IrqSetHandler`; keep them installed.
- kernel_guest_cpu.c: `s_valve` points at a waiter record on a dead thread's stack. Clear it before any device thread polls it.
- kernel_guest_cpu.c: the ticket FIFO must be rebuilt with only the boot thread.
- kernel_guest_det.c: device threads keep running, so `s_dev` slots keep `present`. Only the guest slots and step counters reset.
- kernel_prof.c: `s_watch_esp` points into the TLS of a thread that may have exited. Zero it, with `s_watch_routine`, before the profiler's watch reads it.
- kernel_rtl.c: `g_cs_single` (RECOMP_CS_MODE=single) is one recursive lock an exiting thread may hold. It needs a new lock and a rerun of `g_cs_single_once`.
- kernel_rtl.c: the `g_cs_table` entries of held sections are abandoned, not deleted.
- kernel_sync.c: `g_timer_queue` timers call guest DPCs on host timer-queue threads. Delete them with a blocking wait before band 7 clears the KTIMERs.
- kernel_bridge.c: `g_ke_shadow` and `s_handle_table` hold host CS/handles keyed by guest VA, possibly held by exited threads; the timer and vbl-ack threads keep running, so empty `g_timers`/`g_dpc_queue` while they are paused.
- kernel_bridge.c: `eeprom_va` and the launch page live in the guest heap (re-made after its reset); the band 9 split must leave out `xbox_DpcMarkGuestThread`/`guest_cpu_join` (host thread state).
- xbox_memory_layout.c: band 7 must skip the NV2A/MCPX/flash apertures (APU NOACCESS, AC97 read-only), lift `g_guest_prot` restrictions first, and rewrite the fake PE page at 0x80010000 (inside the contig window).
- xbox_memory_layout.c: frame/fence/counter mirrors are game registrations (no re-register, only bump state resets); `s_watchdog_*` point into the boot thread TLS, valid because that thread is reused.
- guest_vmem.c: decommit the `s_pages` entries in place, keeping the reservation.
- kernel_ramfs.c: the reseed must not re-run a game's save seed (D6). It only rebuilds the RAM cache from the host files.
- kernel_file.c: `s_saves_h` handles are closed by the saves hook, after the thaw.
- xbe_sections.c: the raw section copies could stay; only the resident flags and bytes reset, and the sections are reloaded.
- kernel_xbox.c, kernel_hal.c data exports: the host-native copies are unused by the recompiled path (the bridge has guest copies). Constant.

### NV2A walker, pushbuffer executor and shared backend code

- src/kernel/nv2a_backend_common.c: nv2a_cap_reset() NULLs s_cap_k, which is set once at init (xbox_memory_layout.c); the reset must keep it. The batch_filter_ask memo is keyed by (prog_dwords, start), not content, and is a local static: expose a clear.
- src/kernel/nv2a_dbg_draw.c: the flip thread reads the lists; hide them (nv2a_dbg_hide), never reset the triple-buffer indices.
- src/kernel/nv2a_pb_scan.c: only the walker carry-over (s_left, s_subch, s_method, s_noninc, s_in_call) resets; survey and stats stay.
- src/kernel/nv2a_pb_exec.c: the `inited` one-shot mixes guest defaults (s_gpu min/max, s_vsh.inl w=1) with backend registration; split it and rerun only the defaults. s_flip_hold reset must keep `.edge`, because the cached `mode` never re-arms it.
- src/kernel/nv2a_pb_exec.c: s_gpu mixes registers with counters (flips, draws); zeroing them wraps the delta locals (pacing prev, last_draws, FLIP_LOG prevs). Bump s_tex_gen, s_vsh_cgen and s_scanout_gen, never zero them; s_ff_serial must not reset. s_tc and s_surf_wr_seq reset together.
- src/nv2a/*: nv2a_hook_init is called only from the README, so this library looks unused by our games; classes given for completeness.
- Overlay, present chain, stats, tex_dump and tex_pack are host-only (UI, content-keyed caches, debug catalogs); all keep.

### HLE D3D8, translators, shader cache, D3D11 and Metal backends

- **d3d8_device.c** (Windows HLE D3D8): `d3d8_CreateDevice` memsets `g_device_state` and makes a new D3D11 device and swap chain on every call. A new boot that calls CreateDevice again would recreate the device. The hle_d3d8 hook has to make CreateDevice reuse the device when `g_device_initialized` is set, resetting only the render states, transforms and bindings. `g_device_state` mixes host fields (device, context, swap chain, default RTV/DSV, hwnd, size) with guest state, so the reset clears the guest fields only.
- **d3d8_gl.c** (POSIX HLE D3D8): `g` mixes the SDL window, GL context, VAO/VBO and program (keep) with the D3D state cache and bindings (reset). The same CreateDevice problem applies: it calls SDL_CreateWindow every time. `g_suppress_present` is written by nv2a_pb_replay.c, so its owner is pgraph.
- **d3d8_combiners.c, d3d8_vsh.c, d3d8_shaders.c, d3d8_states.c**: the compiled-shader caches are keyed by content (FNV over the combiner state or the microcode, the FVF, the texture signature), so they are kept. `g_vsh_slots` holds the guest's vertex shader handles, so it is reset.
- **d3d8_gamma.c**: the gamma resources are part of the present chain and are kept. The ramp the guest set is reset to identity.
- **metal_compile.m**: host-only and built once under dispatch_once. Shared with the present library (fb_present_metal.m), so it is never reset.
- **nv2a_pb_d3d11.c, shared memos**: several memos key on PGRAPH generation counters instead of content: `vertex_shader/memo` (prog_dwords, start), `build_stages/memo` (nv2a_pb_tex_regs_gen, contig bytes), `d3d11_on_draw/consts_gen` (nv2a_pb_vsh_consts_gen) and `setup_pixel/lm` (flips). If the pgraph hook zeroes those counters, a stale memo can hit after the reset and draw the wrong shader or old constants. Either clear these memos or keep the pgraph counters monotonic across the reset. Metal has the same issue in `vs_cache/memo`, `build_stages/memo` and `s_vc1`. nv2a_backend_common.c:1684 has another one, in another group.
- **nv2a_pb_d3d11.c, pairs that must move together**: `s_last_rc` and `setup_pixel/memo` must be reset together or kept together. A memo hit with `ps_ok=0` compiles from `s_last_rc`. The generation counters (`s_rt_gen`, `s_tc_gen`, `s_stage_gen`, `s_state_gen`) must be bumped, never zeroed, because memos compare against them. Dropping through `rt_release`/`tex_free` already bumps them. `s_sc` (the bound-state filter) must be cleared together with `ID3D11DeviceContext_ClearState` once targets are released. `s_occ` holds pending reports at guest VAs: its queries must go back to `s_occ_pool` before it is cleared, or the pool leaks.
- **nv2a_pb_metal.m, write-back**: Metal writes dirty render targets back to guest memory (on eviction, on a texture decode, or at every flip under `wb_always`). D3D11 never writes back. The renderer band (1) must drop `s_rt` without a write-back, and before band 7, so that no late write-back lands in the cleared memory or in the new boot's memory. Drain order: `end_encoder`, commit and wait on `s_cmd` and `s_last_cmd`, then release the targets. `s_guard` (debug, `RECOMP_METAL_FB_GUARD`) leaves guest fb pages PROT_NONE, so it must be disarmed (mprotect RW) before band 7's in-place clear.
- **The two backends keep different things** (they should agree, so these are flagged):
  - Metal adopts its device and queue from the window (fb_present_metal.m), and its present chain lives there. D3D11 owns the device, swap chain, librashader chain and overlay in this file, so the D3D11 hook keeps more objects locally.
  - Metal tracks occlusion visibility slots as well (`s_vis_used`, `s_retire`; reset). D3D11 has only the query pool.
  - D3D11 has `s_sc` and `setup_pixel/lm`, which Metal lacks. Metal's equivalent is `s_enc_serial`, which is monotonic and kept.
  - The texture cache sizes already differ (512 entries against 256). This is not a reset matter.

### Devices: APU, DSP, audio, input, USB, video, platform

- src/apu/apu_core.c: APU guest windows are a band 9 re-init: MCPX aperture committed and zeroed at src/kernel/xbox_memory_layout.c:4646, trap pages armed at :4707, AC97 codec-ready bit written at :4714 (rewrite after band 7 clears it); device init at apu_core.c:1599, mcpx_apu_reset_locked at :1562 and apu_diag_reset at :1590 are the reusable parts.
- src/apu/apu_core.c: tib_ready (:368) is APU_TLS on the running frame thread and points at a guest TIB; the hook cannot reach it, so it needs a generation counter.
- src/apu/apu_dsp_run.c: s (:94) mixes DSP core state with kept worker threads and env; reset only the cores (existing memset at :372 keeps on and stats); cores bind to the MCPX window at :331.
- src/apu/apu_dsp.c: mcpx_apu_dsp_init (:326) already resets the ack and boot statics, but also re-reads env; split it for the hook.
- src/apu/apu_vp.c: mcpx_apu_vp_finalize (:2005) clears s_vdump, s_ring, s_vstat; mcpx_apu_vp_reset clears d->vp.
- src/input/host_open.c: host-only menu request; drained in band 4 per spec, not guest state.
- src/input/xinput_device.c: g_slot keeps host pads; its xbox_rumble_gate holds the guest rumble request, zero it in the input hook.
- src/usb/ohci.c: xbox_OhciInit (:1779) is guarded by done; the hook must call ohci_reset per controller and clear s_plugged, s_gone and the control pipe statics without re-protecting trap pages or re-creating the thread.
- src/usb/ohci.c: s_det_ms must re-read after the band 10 clock reset, or the deterministic pad clock jumps.
- src/video/fb_present.c: s_present keeps the last frame on screen until the new boot flips; the guest framebuffer and PVIDEO snapshot are reset.
- src/platform/win32_compat.c: POSIX emulation of Win32 host objects; nothing here is guest state.

### Enhancements, overlay and mods layer

- enhance_actions.c: `enhance_actions_reset()` wipes the whole registry, bindings table and arm list (and unhooks the pad filter and flip hook). A game reset must never call it. The registry is the kept action table. `s_port` holds host pad state and goes on unchanged.
- enhance_input.c: `s_mod_capture` is the free camera's (or any mod's) pad and mouse divert. The mods hook releases it through `enhance_input_capture(0)`, which also keeps `s_div_lock` in step. A mod's `on_reset` takes it again if it needs to.
- enhance_menu.c: everything is kept. The menu triggers the Restart and stays open across it. `s_rate_c0` is a snapshot of the nv2a stat totals: if any NV2A_STAT counter is zeroed at the reset, the next one-second rate goes negative or wrong once. Re-baseline it then (set `s_rate_t0 = 0`).
- hud_anchor.c: `t_chain`/`t_save` are thread-locals keyed on guest esp. `t_save` writes the saved x/u back into guest memory at a matching post. `hud_layer_reset()` clears only the calling thread's copy. The boot thread is reused across boots, so the boot loop must clear its copy on the boot thread itself; dead guest threads take theirs with them. `hud_layer_reset()` also drops the table, so the enhance hook needs a narrower helper: `s_on`/`s_mod` to 0 plus `nv2a_aspect_hud_set(0,0,0)`, and `s_snap` cleared with a seqlock write.
- hud_anchor.c: the table's `[[wrapper]]` hooks are installed through `api_hook` under the owner mod, so they live in mod.c's `s_hooks`. If D6a strips that mod's hooks, the wrappers go too. But `hud_table` is init-only ("one table per run"). So the mods hook needs to keep the HUD owner's wrapper hooks, or re-hook them itself, or allow `hud_table` again from `on_reset`.
- hud_capture.c: a debug tool (`debug.hud_capture`). D7.1 already refuses a reset while it is on, so everything here is kept.
- mod.c: `t_in_guest_cb` is a thread-local depth counter. An unwind from inside a hook on the boot thread leaves it raised, which would let `guest_call`/`guest_alloc` through outside a callback. The boot thread must zero it. `s_galloc_va` points into the contiguous arena, which is cleared: without a reset, `guest_alloc` would hand back a block the new boot's kernel may give out again.
- mod.c: `api_hook` and `api_hud_table` refuse unless `s_init_owner` is set. For `on_reset` to re-install hooks (D6a), the mods hook must set `s_init_owner = i` around each call, as `xbox_mod_init` does. The hook must also call `recomp_hook_drop_owner(&s_mod[i])` and re-run `recomp_hook_publish()`, which is documented "called once".

### What the mod API records today about guest hooks and pokes (for D6a `needs`)

- **Hooks: recorded, but the VA lives in only one place.** `api_hook` (init-only, via `s_init_owner`) appends `{pre, post, user, owner}` to `s_hooks[64]` and queues the VA with `recomp_hook_add(&s_mod[i], name, va, ...)` in src/kernel/recomp_hook.c. The owner token is `&s_mod[i]`. `s_hooks` itself does not store the VA. `recomp_hook_drop_owner(owner)` exists (used for a failed init), and `recomp_hook_publish()` builds the chains once. The HUD capture's hooks are separate (`s_cap_hooks`, owner `&s_cap_owner`). So a per-mod hook list for `needs` checking can come from the queued hooks per owner. Nothing compares hooks against `needs & XBOX_MOD_NEED_HOOK` today.
- **Pokes and state reads: not recorded at all.** A mod writes and reads guest memory directly through `api->mem + va`. There is no poke or state helper in the API to intercept. `XBOX_MOD_NEED_POKE`/`_STATE`/`_ASPECT`/`_PLACE` are declared in mod.h, and `struct xbox_mod.needs` exists (v4), but mod.c never reads `needs`: mod.h says "nothing reads it until the enable gate (mod-connectivity-layer task 4.2) lands". D6a's "seen using POKE/STATE" therefore needs new API helpers (or `needs` taken as declared). Direct `mem` writes stay invisible.
- **Other guest-derived per-mod records:** `s_galloc_va`/`s_galloc_size` (one guest_alloc block per mod, never freed), `s_hud_owner` (HUD table owner), `s_active` (last `api->active`), `s_status` panel text, and the open 3D debug frame (`s_dbg_open`/`s_dbg_last`). `guest_call` keeps nothing.

## Scanner rules

What `reset_statics.py` does not list, by design:

- Statics declared by a macro expansion, such as `_once` in the log-once
  macro at `src/nv2a/qemu_shim.h:281`. The scanner skips preprocessor
  lines. Each expansion is a log guard, class keep.
- Two statics with the same name in one function share one row, since the
  key has no line number. In `nv2a_pb_exec_method` this covers two `shown`
  and two `prev_tris`; in `nv2a_pb_exec_report`, several `done`; in
  `mcpx_apu_dsp_ack_poll` (`src/apu/apu_dsp.c`), the scalar and array
  `shown`. All are log guards or trace state, so the one class fits each.
- Fields inside a static table, such as `struct hud_group.ref` and
  `struct hud_site.last_dx` in `src/enhance/hud_anchor.c`. The row of the
  table covers them. `ref` is epoch-stamped and expires on its own while
  `s_epoch` keeps counting, and `last_dx` is trace state.
- A function body whose name the scanner cannot find. It prints a warning
  naming the file and line, so the miss is visible.

## Counts

2559 objects at the phase 0 audit: 2023 keep, 416 reset, 120 constant.
`reset_statics.py check` prints the current totals.
