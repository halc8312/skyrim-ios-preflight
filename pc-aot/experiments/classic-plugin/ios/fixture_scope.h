/* Private to the authored fixture's object target. The game and the probe
 * use the same register definitions, with independent memory and dispatch. */
#pragma once
#define MEM skyrim_serial_i386_memory
#define rt_call skyrim_serial_i386_call
#define rt_call_ic skyrim_serial_i386_call_cached
#define g_fn_table_serial skyrim_serial_i386_functions
#define g_fn_count_serial skyrim_serial_i386_function_count
#define fixture_fail skyrim_serial_i386_fail
#define fixture_register_plugin skyrim_serial_i386_register
#define fixture_invoke_event skyrim_serial_i386_event
#define host_handle skyrim_serial_i386_handle
#define host_unique_id skyrim_serial_i386_owner
#define host_query_interface skyrim_serial_i386_interface
#define host_event skyrim_serial_i386_register_event
#define host_write skyrim_serial_i386_write
#define host_next skyrim_serial_i386_next
#define host_read skyrim_serial_i386_read
