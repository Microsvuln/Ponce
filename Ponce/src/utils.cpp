#include <stdio.h>
#include <string>
#include <iostream>
#include <fstream>

//Triton
#include <api.hpp>
#include "x86Specifications.hpp"

//IDA
#include <idp.hpp>
#include <loader.hpp>
#include <dbg.hpp>
#include <name.hpp>

//Ponce
#include "utils.hpp"
#include "globals.hpp"
#include "formChoser.hpp"

/*This function is call the first time we are tainting something to enable the trigger, the flags and the tracing*/
void start_tainting_or_symbolic_analysis()
{
	if (!is_something_tainted_or_symbolize)
	{
		runtimeTrigger.enable();
		is_something_tainted_or_symbolize = true;
		enable_step_trace(true);
		/*if (ENABLE_STEP_INTO_WHEN_TAINTING)
			automatically_continue_after_step = true;*/
			//enable_insn_trace(true);
	}
}

/*This functions gets a string and return the triton register assign or NULL
This is using the triton current architecture so it is more generic.*/
triton::arch::Register *str_to_register(std::string register_name)
{
	auto regs = triton::api.getAllRegisters();
	for (auto it = regs.begin(); it != regs.end(); it++)
	{
		triton::arch::Register *reg = *it;
		if (reg->getName() == register_name)
			return reg;
	}
	return NULL;
}

/*We need this helper because triton doesn't allow to taint memory regions unalinged, so we taint every byte*/
void taint_all_memory(triton::__uint address, triton::__uint size)
{
	for (unsigned int i = 0; i < size; i++)
	{
		triton::api.taintMemory(address + i);
	}
}

/*We need this helper because triton doesn't allow to symbolize memory regions unalinged, so we symbolize every byte*/
void symbolize_all_memory(triton::__uint address, triton::__uint size, char *comment)
{
	for (unsigned int i = 0; i < size; i++)
	{
		triton::api.convertMemoryToSymbolicVariable(triton::arch::MemoryAccess(address + i, 1, 0), comment);
	}
}

/*This function ask to the user to take a snapshot.
It returns:
1: yes
0: No
-1: Cancel execution of script*/
int ask_for_a_snapshot()
{
	//We don't want to ask the user all the times if he wants to take a snapshot or not...
	if (already_exits_a_snapshot())
		return 1;
	while (true)
	{
		int answer = askyn_c(1, "[?] Do you want to take a database snapshot before using the script? (It will color some intructions) (Y/n):");
		if (answer == 1) //Yes
		{
			snapshot_t snapshot;
			strcpy_s(snapshot.desc, MAX_DATABASE_DESCRIPTION, SNAPSHOT_DESCRIPTION);
			qstring errmsg;
			bool success = take_database_snapshot(&snapshot, &errmsg);
			return 1;
		}
		else if (answer == 0) //No
			return 0;
		else //Cancel
			return -1;
	}
}

/*This functions is a helper for already_exits_a_snapshot. This is call for every snapshot found. 
The user data, ud, containt a pointer to a boolean to enable if we find the snapshot.*/
int __stdcall snapshot_visitor(snapshot_t *ss, void *ud)
{
	if (strcmp(ss->desc, SNAPSHOT_DESCRIPTION) == 0)
	{
		bool *exists = (bool *)ud;
		*exists = true;
		return 1;
	}
	return 0;
}

/*This functions check if it exists already a snapshot made by the plugin.
So we don't ask to the user every time he runs the plugin.*/
bool already_exits_a_snapshot()
{
	snapshot_t root;
	bool result = build_snapshot_tree(&root);
	if (!result)
		return false;
	bool exists = false;
	visit_snapshot_tree(&root, &snapshot_visitor, &exists);
	return exists;
}

/*This function is a helper to find a function having its name.
It is likely IDA SDK has another API to do this but I can't find it.
Source: http://www.openrce.org/reference_library/files/ida/idapw.pdf */
ea_t find_function(char *function_name)
{
	// get_func_qty() returns the number of functions in file(s) loaded.
	for (unsigned int f = 0; f < get_func_qty(); f++)
	{
		// getn_func() returns a func_t struct for the function number supplied
		func_t *curFunc = getn_func(f);
		char funcName[MAXSTR];
		// get_func_name gets the name of a function,
		// stored in funcName
		get_func_name(curFunc->startEA, funcName, sizeof(funcName) - 1);
		if (strcmp(funcName, function_name) == 0)
			return curFunc->startEA;
	}
	return -1;
}

//This function return the real value of the argument.
triton::__uint get_args(int argument_number, bool skip_ret)
{
	triton::__uint memprogram = get_args_pointer(argument_number, skip_ret);
	//We first get the pointer and then we dereference it
	triton::__uint value = 0;
	value = read_uint_from_ida(memprogram);
	return value;
}

// Return the argument at the "argument_number" position. It is independant of the architecture and the OS.
// We suppossed the function is using the default call convention, stdcall or cdelc in x86, no fastcall and fastcall in x64
triton::__uint get_args_pointer(int argument_number, bool skip_ret)
{
	int skip_ret_index = skip_ret ? 1 : 0;
#ifdef X86_32
	regval_t esp_value;
	invalidate_dbg_state(DBGINV_REGS);
	get_reg_val("esp", &esp_value);
	//msg("argument_number: %d\n", argument_number);
	//msg("esp: "HEX_FORMAT"\n", (unsigned int)esp_value.ival);
	triton::__uint arg = (triton::__uint)esp_value.ival + (argument_number + skip_ret_index) * 4;
	//msg("arg: "HEX_FORMAT"\n", arg);
	return arg;
#elif X86_64
	//Not converted to IDA we should use get_reg_val
#ifdef _WIN32 // note the underscore: without it, it's not msdn official!
	// On Windows - function parameters are passed in using RCX, RDX, R8, R9 for ints / ptrs and xmm0 - 3 for float types.
	switch (argument_number)
	{
	case 0: return getCurrentRegisterValue(TRITON_X86_REG_RCX).convert_to<__uint>();
	case 1: return getCurrentRegisterValue(TRITON_X86_REG_RDX).convert_to<__uint>();
	case 2: return getCurrentRegisterValue(TRITON_X86_REG_R8).convert_to<__uint>();
	case 3: return getCurrentRegisterValue(TRITON_X86_REG_R9).convert_to<__uint>();
	default:
		__uint esp = (__uint)getCurrentRegisterValue(TRITON_X86_REG_RSP).convert_to<__uint>();
		__uint arg = esp + (argument_number - 4 + skip_ret_index) * 8;
		return *(__uint*)arg;
	}
#elif __unix__
	//On Linux - parameters are passed in RDI, RSI, RDX, RCX, R8, R9 for ints / ptrs and xmm0 - 7 for float types.
	switch (argument_number)
	{
	case 0: return getCurrentRegisterValue(TRITON_X86_REG_RDI).convert_to<__uint>();
	case 1: return getCurrentRegisterValue(TRITON_X86_REG_RSI).convert_to<__uint>();
	case 2: return getCurrentRegisterValue(TRITON_X86_REG_RDX).convert_to<__uint>();
	case 3: return getCurrentRegisterValue(TRITON_X86_REG_RCX).convert_to<__uint>();
	case 4: return getCurrentRegisterValue(TRITON_X86_REG_R8).convert_to<__uint>();
	case 5: return getCurrentRegisterValue(TRITON_X86_REG_R9).convert_to<__uint>();
	default:
		__uint esp = (__uint)getCurrentRegisterValue(TRITON_X86_REG_RSP);
		__uint arg = esp + (argument_number - 6 + skip_ret_index) * 8;
		return *(__uint*)arg;
	}
#endif
#endif
}

//Use templates??
char read_char_from_ida(ea_t address)
{
	//msg("read_char_from_ida: "HEX_FORMAT"\n", address);
	char value;
	//This is the way to force IDA to read the value from the debugger
	//More info here: https://www.hex-rays.com/products/ida/support/sdkdoc/dbg_8hpp.html#ac67a564945a2c1721691aa2f657a908c
	invalidate_dbgmem_contents(address, sizeof(value));
	if (!get_many_bytes(address, &value, sizeof(value)))
		warning("Error reading memory from "HEX_FORMAT"\n", address);
	return value;
}

triton::__uint read_uint_from_ida(ea_t address)
{
	//msg("read_uint_from_ida: "HEX_FORMAT"\n", address);
	triton::__uint value;
	//This is the way to force IDA to read the value from the debugger
	//More info here: https://www.hex-rays.com/products/ida/support/sdkdoc/dbg_8hpp.html#ac67a564945a2c1721691aa2f657a908c
	invalidate_dbgmem_contents(address, sizeof(value));
	if (!get_many_bytes(address, &value, sizeof(value)))
		warning("Error reading memory from "HEX_FORMAT"\n", address);
	return value;
}

/*This function renames a tainted function with the prefix RENAME_TAINTED_FUNCTIONS_PREFIX, by default "T%03d_"*/
void rename_tainted_function(ea_t address)
{
	char func_name[MAXSTR];
	//First we get the current function name
	if (get_func_name(address, func_name, sizeof(func_name)) != NULL)
	{
		//If the function isn't already renamed
		if (strstr(func_name, "T0") != func_name)
		{
			char new_func_name[MAXSTR];
			//This is a bit tricky, the prefix contains the format string, so if the user modified it and removes the format string isn't going to work
			sprintf_s(new_func_name, sizeof(new_func_name), RENAME_TAINTED_FUNCTIONS_PREFIX"_%s", tainted_functions_index, func_name);
			//We need the start of the function we can have that info with our function find_function
			set_name(find_function(func_name), new_func_name);
			if (cmdOptions.showDebugInfo)
				msg("[+] Renaming function %s -> %s\n", func_name, new_func_name);
			tainted_functions_index += 1;
		}
	}
}

void add_symbolic_expressions(triton::arch::Instruction* tritonInst, ea_t address)
{
	for (unsigned int exp_index = 0; exp_index != tritonInst->symbolicExpressions.size(); exp_index++) 
	{
		auto expr = tritonInst->symbolicExpressions[exp_index];
		std::ostringstream oss;
		oss << expr;
		add_long_cmt(address, false, oss.str().c_str());
	}
}

std::string notification_code_to_string(int notification_code)
{
	switch (notification_code)
	{
		case 0:
			return std::string("dbg_null");
		case 1:
			return std::string("dbg_process_start");
		case 2:
			return std::string("dbg_process_exit");
		case 3:
			return std::string("dbg_process_attach");
		case 4:
			return std::string("dbg_process_detach");
		case 5:
			return std::string("dbg_thread_start");
		case 6:
			return std::string("dbg_thread_exit");
		case 7:
			return std::string("dbg_library_load");
		case 8:
			return std::string("dbg_library_unload");
		case 9:
			return std::string("dbg_information");
		case 10:
			return std::string("dbg_exception");
		case 11:
			return std::string("dbg_suspend_process");
		case 12:
			return std::string("dbg_bpt");
		case 13:
			return std::string("dbg_trace");
		case 14:
			return std::string("dbg_request_error");
		case 15:
			return std::string("dbg_step_into");
		case 16:
			return std::string("dbg_step_over");
		case 17:
			return std::string("dbg_run_to");
		case 18:
			return std::string("dbg_step_until_ret");
		case 19:
			return std::string("dbg_bpt_changed");
		case 20:
			return std::string("dbg_last");
		default:
			return std::string("Not defined");
	}
}

/*This function loads the options from the config file.
It returns true if it reads the config false, if there is any error.*/
bool load_options(struct cmdOptionStruct *cmdOptions)
{
	std::ifstream config_file;
	config_file.open("Ponce.cfg", std::ios::in | std::ios::binary);
	if (!config_file.is_open())
	{
		msg("Config file %s not found\n", "Ponce.cfg");
		return false;
	}
	auto begin = config_file.tellg();
	config_file.seekg(0, std::ios::end);
	auto end = config_file.tellg();
	config_file.seekg(0, std::ios::beg);
	if ((end - begin) != sizeof(struct cmdOptionStruct))
		return false;
	config_file.read((char *)cmdOptions, sizeof(struct cmdOptionStruct));
	config_file.close();
	return true;
}

/*This function loads the options from the config file.
It returns true if it reads the config false, if there is any error.*/
bool save_options(struct cmdOptionStruct *cmdOptions)
{
	std::ofstream config_file;
	config_file.open("Ponce.cfg", std::ios::out | std::ios::binary);
	if (!config_file.is_open())
	{
		msg("Error opening config file %s\n", "Ponce.cfg");
		return false;
	}
	config_file.write((char *)cmdOptions, sizeof(struct cmdOptionStruct));
	config_file.close();
	return true;
}


/*returns true if a formula was solved*/
Input * solve_formula(ea_t pc, uint bound){
	for (unsigned int i = myPathConstraints.size() - 1; i >= 0; i--)
	{
		auto path_constraint = myPathConstraints[i];
		if (path_constraint.conditionAddr == pc)
		{
			std::vector <triton::ast::AbstractNode *> expr;
			//First we add to the expresion all the previous path constrains
			unsigned int j;
			for (j = 0; j < i; j++)
			{
				if (cmdOptions.showExtraDebugInfo)
					msg("Keeping condition %d\n", j);
				triton::__uint ripId = myPathConstraints[j].conditionRipId;
				auto symExpr = triton::api.getFullAstFromId(ripId);
				triton::__uint takenAddr = myPathConstraints[j].takenAddr;
				expr.push_back(triton::ast::assert_(triton::ast::equal(symExpr, triton::ast::bv(takenAddr, symExpr->getBitvectorSize()))));
			}
			if (cmdOptions.showExtraDebugInfo)
				msg("Inverting condition %d\n", i);
			//And now we negate the selected condition
			triton::__uint ripId = myPathConstraints[i].conditionRipId;
			auto symExpr = triton::api.getFullAstFromId(ripId);
			triton::__uint notTakenAddr = myPathConstraints[i].notTakenAddr;
			if (cmdOptions.showExtraDebugInfo)
				msg("ripId: %d notTakenAddr: "HEX_FORMAT"\n", ripId, notTakenAddr);
			expr.push_back(triton::ast::assert_(triton::ast::equal(symExpr, triton::ast::bv(notTakenAddr, symExpr->getBitvectorSize()))));

			//Time to solve
			auto final_expr = triton::ast::compound(expr);

			if (cmdOptions.showDebugInfo)
				msg("[+] Solving formula...\n");

			if (cmdOptions.showExtraDebugInfo){
				std::stringstream ss;
				/*Create the full formula*/
				ss << "(set-logic QF_AUFBV)\n";
				/* Then, delcare all symbolic variables */
				ss << triton::api.getVariablesDeclaration();
				/* And concat the user expression */
				ss << "\n\n";
				ss << final_expr;
				ss << "\n(check-sat)";
				ss << "\n(get-model)";
				msg("Formula: %s\n", ss.str().c_str());
			}

			auto model = triton::api.getModel(final_expr);

			if (model.size() > 0)
			{
				Input *newinput = new Input();
				//Clone object 
				newinput->bound = path_constraint.bound;

				msg("Solution found! Values:\n");
				for (auto it = model.begin(); it != model.end(); it++)
				{
					auto symbVar = triton::api.getSymbolicVariableFromId(it->first);
					std::string  symbVarComment = symbVar->getComment();
					triton::engines::symbolic::symkind_e symbVarKind = symbVar->getKind();
					triton::uint512 secondValue = it->second.getValue();
					if (symbVarKind == triton::engines::symbolic::symkind_e::MEM){
						msg("memory %x \n", symbVar->getKindValue());
						newinput->memOperand.push_back(triton::arch::MemoryAccess(symbVar->getKindValue(), symbVar->getSize() / 8, secondValue));
						msg("newinput->memOperand size %d\n", newinput->memOperand.size());
					}
					else if (symbVarKind == triton::engines::symbolic::symkind_e::REG){
						warning("register");
						newinput->regOperand.push_back(triton::arch::Register((triton::__uint)symbVar->getKindValue(), secondValue));
					}
					//We represent the number different 
					switch (symbVar->getSize())
					{
					case 8:
						msg(" - %s (%s):%#02x (%c)\n", it->second.getName().c_str(), symbVarComment.c_str(), secondValue.convert_to<uchar>(), secondValue.convert_to<uchar>());
						break;
					case 16:
						msg(" - %s (%s):%#04x\n", it->second.getName().c_str(), symbVarComment.c_str(), secondValue.convert_to<ushort>());
						break;
					case 32:
						msg(" - %s (%s):%#08x\n", it->second.getName().c_str(), symbVarComment.c_str(), secondValue.convert_to<uint>());
						break;
					case 64:
						msg(" - %s (%s):%#16llx\n", it->second.getName().c_str(), symbVarComment.c_str(), secondValue.convert_to<uint64>());
						break;
					default:
						msg("Unsupported size for the symbolic variable: %s (%s)\n", it->second.getName().c_str(), symbVarComment.c_str());
					}
				}
				return newinput;
			}
			else
				msg("No solution found :(\n");

			break;
		}

	}
	return NULL;
}



/*This function identify the type of condition jmp and negate the flags to negate the jmp.
Probably it is possible to do this with the solver, adding more variable to the formula to 
identify the flag of the conditions and get the values. But for now we are doing it in this way.*/
void negate_flag_condition(triton::arch::Instruction *triton_instruction)
{
	switch (triton_instruction->getType())
	{
	case triton::arch::x86::ID_INS_JA:
	{
		uint64 cf;
		get_reg_val("CF", &cf);
		uint64 zf;
		get_reg_val("ZF", &zf);
		if (cf == 0 && zf == 0)
		{
			cf = 1;
			zf = 1;
		}
		else
		{
			cf = 0;
			zf = 0;
		}
		set_reg_val("ZF", zf);
		set_reg_val("CF", cf);
		break;
	}
	case triton::arch::x86::ID_INS_JAE:
	{
		uint64 cf;
		get_reg_val("CF", &cf);
		uint64 zf;
		get_reg_val("ZF", &zf);
		if (cf == 0 || zf == 0)
		{
			cf = 1;
			zf = 1;
		}
		else
		{
			cf = 0;
			zf = 0;
		}
		set_reg_val("ZF", zf);
		set_reg_val("CF", cf);
		break;
	}
	case triton::arch::x86::ID_INS_JB:
	{
		uint64 cf;
		get_reg_val("CF", &cf);
		cf = !cf;
		set_reg_val("CF", cf);
		break;
	}
	case triton::arch::x86::ID_INS_JBE:
	{
		uint64 cf;
		get_reg_val("CF", &cf);
		uint64 zf;
		get_reg_val("ZF", &zf);
		if (cf == 1 || zf == 1)
		{
			cf = 0;
			zf = 0;
		}
		else
		{
			cf = 1;
			zf = 1;
		}
		set_reg_val("ZF", zf);
		set_reg_val("CF", cf);
		break;
	}
	/*	ToDo: Check this one
		case triton::arch::x86::ID_INS_JCXZ:
		{
		break;
		}*/
	case triton::arch::x86::ID_INS_JE:
	{
		uint64 zf;
		auto old_value = get_reg_val("ZF", &zf);
		zf = !zf;
		set_reg_val("ZF", zf);
		break;
	}
	/*case triton::arch::x86::ID_INS_JECXZ:
	{
	break;
	}*/
	case triton::arch::x86::ID_INS_JG:
	{
		uint64 sf;
		get_reg_val("SF", &sf);
		uint64 of;
		get_reg_val("OF", &of);
		uint64 zf;
		get_reg_val("ZF", &zf);
		if (sf == of && zf == 0)
		{
			sf = !of;
			zf = 1;
		}
		else
		{
			sf = of;
			zf = 0;
		}
		set_reg_val("SF", sf);
		set_reg_val("OF", of);
		set_reg_val("ZF", zf);
		break;
	}
	case triton::arch::x86::ID_INS_JGE:
	{
		break;
	}
	case triton::arch::x86::ID_INS_JL:
	{
		break;
	}
	case triton::arch::x86::ID_INS_JLE:
	{
		break;
	}
	case triton::arch::x86::ID_INS_JNE:
	{
		uint64 val;
		auto old_value = get_reg_val("ZF", &val);
		val = !val;
		set_reg_val("ZF", val);
		break;
	}
	case triton::arch::x86::ID_INS_JNO:
	{
		break;
	}
	case triton::arch::x86::ID_INS_JNP:
	{
		break;
	}
	case triton::arch::x86::ID_INS_JNS:
	{
		break;
	}
	case triton::arch::x86::ID_INS_JO:
	{
		break;
	}
	case triton::arch::x86::ID_INS_JP:
	{
		break;
	}
	case triton::arch::x86::ID_INS_JRCXZ:
	{
		break;
	}
	case triton::arch::x86::ID_INS_JS:
	{
		break;
	}
	}
}

/*Ask to the user if he really want to execute native code even if he has a snapshot.
Returns true if the user say yes.*/
bool ask_for_execute_native()
{
	//Is there any snapshot?
	if (!snapshot.exists())
		return true;
	//If so we should say to the user that he cannot execute native code and expect the snapshot to work

	int answer = askyn_c(1, "[?] If you execute native code (without tracing) Ponce cannot trace all the memory modification and the snapshot won't work. Do you still want to do it? (Y/n):");
	if (answer == 1) //Yes
		return true;
	else // No or Cancel
		return false;
}