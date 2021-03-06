#include "stdafx.h"
#include "Utilities/Log.h"
#include "Emu/System.h"
#include "Emu/Memory/Memory.h"
#include "Emu/CPU/CPUDecoder.h"

#include "ARMv7Thread.h"
#include "PSVFuncList.h"
#include "ARMv7Interpreter.h"

#define reject(cond, info) { if (cond) Error(__FUNCTION__, code, type, #cond, info); }

namespace ARMv7_instrs
{
	template<typename T>
	u32 BitCount(T x, size_t len = sizeof(T) * 8)
	{
		u32 result = 0;

		for (T mask = static_cast<T>(1) << (len - 1); mask; mask >>= 1)
		{
			if (x & mask) result++;
		}

		return result;
	}

	//template<typename T>
	//s8 LowestSetBit(T x, u8 len)
	//{
	//	if (!x) return len;

	//	u8 result = 0;

	//	for (T mask = 1, i = 0; i<len && (x & mask) == 0; mask <<= 1, i++)
	//	{
	//		result++;
	//	}

	//	return result;
	//}

	//template<typename T>
	//s8 HighestSetBit(T x, u8 len)
	//{
	//	if (!x) return -1;

	//	u8 result = len;

	//	for (T mask = T(1) << (len - 1); (x & mask) == 0; mask >>= 1)
	//	{
	//		result--;
	//	}

	//	return result;
	//}

	//template<typename T>
	//s8 CountLeadingZeroBits(T x, u8 len)
	//{
	//	return len - 1 - HighestSetBit(x, len);
	//}

	SRType DecodeImmShift(u32 type, u32 imm5, u32* shift_n)
	{
		SRType shift_t = SRType_None;

		switch (type)
		{
		case 0: shift_t = SRType_LSL; if (shift_n) *shift_n = imm5; break;
		case 1: shift_t = SRType_LSR; if (shift_n) *shift_n = imm5 == 0 ? 32 : imm5; break;
		case 2: shift_t = SRType_ASR; if (shift_n) *shift_n = imm5 == 0 ? 32 : imm5; break;
		case 3:
			if (imm5 == 0)
			{
				shift_t = SRType_RRX; if (shift_n) *shift_n = 1;
			}
			else
			{
				shift_t = SRType_ROR; if (shift_n) *shift_n = imm5;
			}
			break;
		}

		return shift_t;
	}

	SRType DecodeRegShift(u8 type)
	{
		SRType shift_t = SRType_None;

		switch (type)
		{
		case 0: shift_t = SRType_LSL; break;
		case 1: shift_t = SRType_LSR; break;
		case 2: shift_t = SRType_ASR; break;
		case 3: shift_t = SRType_ROR; break;
		}

		return shift_t;
	}

	u32 LSL_C(u32 x, s32 shift, bool& carry_out)
	{
		assert(shift > 0);
		carry_out = shift <= 32 ? x & (1 << (32 - shift)) : false;
		return shift < 32 ? x << shift : 0;
	}

	u32 LSL_(u32 x, s32 shift)
	{
		assert(shift >= 0);
		return shift < 32 ? x << shift : 0;
	}

	u32 LSR_C(u32 x, s32 shift, bool& carry_out)
	{
		assert(shift > 0);
		carry_out = shift <= 32 ? x & (1 << (shift - 1)) : false;
		return shift < 32 ? x >> shift : 0;
	}

	u32 LSR_(u32 x, s32 shift)
	{
		assert(shift >= 0);
		return shift < 32 ? x >> shift : 0;
	}

	s32 ASR_C(s32 x, s32 shift, bool& carry_out)
	{
		assert(shift > 0);
		carry_out = shift <= 32 ? x & (1 << (shift - 1)) : false;
		return shift < 32 ? x >> shift : x >> 31;
	}

	s32 ASR_(s32 x, s32 shift)
	{
		assert(shift >= 0);
		return shift < 32 ? x >> shift : x >> 31;
	}

	u32 ROR_C(u32 x, s32 shift, bool& carry_out)
	{
		assert(shift);
		carry_out = x & (1 << (shift - 1));
		return x >> shift | x << (32 - shift);
	}

	u32 ROR_(u32 x, s32 shift)
	{
		return x >> shift | x << (32 - shift);
	}

	u32 RRX_C(u32 x, bool carry_in, bool& carry_out)
	{
		carry_out = x & 0x1;
		return ((u32)carry_in << 31) | (x >> 1);
	}

	u32 RRX_(u32 x, bool carry_in)
	{
		return ((u32)carry_in << 31) | (x >> 1);
	}

	u32 Shift_C(u32 value, u32 type, s32 amount, bool carry_in, bool& carry_out)
	{
		assert(type != SRType_RRX || amount == 1);

		if (amount)
		{
			switch (type)
			{
			case SRType_LSL: return LSL_C(value, amount, carry_out);
			case SRType_LSR: return LSR_C(value, amount, carry_out);
			case SRType_ASR: return ASR_C(value, amount, carry_out);
			case SRType_ROR: return ROR_C(value, amount, carry_out);
			case SRType_RRX: return RRX_C(value, carry_in, carry_out);
			default: throw __FUNCTION__;
			}
		}

		carry_out = carry_in;
		return value;
	}

	u32 Shift(u32 value, u32 type, s32 amount, bool carry_in)
	{
		bool carry_out;
		return Shift_C(value, type, amount, carry_in, carry_out);
	}

	template<typename T> T AddWithCarry(T x, T y, bool carry_in, bool& carry_out, bool& overflow)
	{
		const T sign_mask = (T)1 << (sizeof(T) - 1);

		T result = x + y;
		carry_out = ((x & y) | ((x ^ y) & ~result)) & sign_mask;
		overflow = (x ^ result) & (y ^ result) & sign_mask;
		if (carry_in)
		{
			result += 1;
			carry_out ^= (result == 0);
			overflow ^= (result == sign_mask);
		}
		return result;
	}

	u32 ThumbExpandImm_C(u32 imm12, bool carry_in, bool& carry_out)
	{
		if ((imm12 & 0xc00) >> 10)
		{
			u32 unrotated_value = (imm12 & 0x7f) | 0x80;

			return ROR_C(unrotated_value, (imm12 & 0xf80) >> 7, carry_out);
		}
		else
		{
			carry_out = carry_in;

			u32 imm8 = imm12 & 0xff;
			switch ((imm12 & 0x300) >> 8)
			{
			case 0: return imm8;
			case 1: return imm8 << 16 | imm8;
			case 2: return imm8 << 24 | imm8 << 8;
			default: return imm8 << 24 | imm8 << 16 | imm8 << 8 | imm8;
			}
		}
	}

	u32 ThumbExpandImm(ARMv7Context& context, u32 imm12)
	{
		bool carry = context.APSR.C;
		return ThumbExpandImm_C(imm12, carry, carry);
	}

	bool ConditionPassed(ARMv7Context& context, u32 cond)
	{
		bool result = false;

		switch (cond >> 1)
		{
		case 0: result = context.APSR.Z == 1; break;
		case 1: result = context.APSR.C == 1; break;
		case 2: result = context.APSR.N == 1; break;
		case 3: result = context.APSR.V == 1; break;
		case 4: result = context.APSR.C == 1 && context.APSR.Z == 0; break;
		case 5: result = context.APSR.N == context.APSR.V; break;
		case 6: result = context.APSR.N == context.APSR.V && context.APSR.Z == 0; break;
		case 7: return true;
		}

		if (cond & 0x1)
		{
			return !result;
		}

		return result;
	}

	void Error(const char* func, const ARMv7Code code, const ARMv7_encoding type, const char* cond, const char* info)
	{
		auto format_encoding = [](const ARMv7_encoding type) -> const char*
		{
			switch (type)
			{
			case T1: return "T1";
			case T2: return "T2";
			case T3: return "T3";
			case T4: return "T4";
			case A1: return "A1";
			case A2: return "A2";
			default: return "???";
			}
		};

		throw fmt::format("%s(%s) error: %s (%s)", func, format_encoding(type), info, cond);
	}
}

void ARMv7_instrs::UNK(ARMv7Context& context, const ARMv7Code code)
{
	throw fmt::format("Unknown/illegal opcode: 0x%04x 0x%04x", code.code1, code.code0);
}

void ARMv7_instrs::NULL_OP(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	throw fmt::format("Null opcode found: 0x%04x 0x%04x", code.code1, code.code0);
}

void ARMv7_instrs::HACK(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, func;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		func = code.data & 0xffff;
		break;
	}
	case A1:
	{
		cond = code.data >> 28;
		func = (code.data & 0xfff00) >> 4 | (code.data & 0xf);
		break;
	}
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		execute_psv_func_by_index(context, func);
	}
}

void ARMv7_instrs::MRC_(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, t, cp, opc1, opc2, cn, cm;

	switch (type)
	{
	case T1:
	case A1:
	{
		cond = context.ITSTATE.advance();
		t = (code.data & 0xf000) >> 12;
		cp = (code.data & 0xf00) >> 8;
		opc1 = (code.data & 0xe00000) >> 21;
		opc2 = (code.data & 0xe0) >> 5;
		cn = (code.data & 0xf0000) >> 16;
		cm = (code.data & 0xf);

		reject(cp - 10 < 2, "Advanced SIMD and VFP");
		reject(t == 13 && type == T1, "UNPREDICTABLE");
		break;
	}
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		if (cp == 15 && opc1 == 0 && cn == 13 && cm == 0 && opc2 == 3)
		{
			LOG_ERROR(ARMv7, "TODO: TLS requested");

			if (t < 15)
			{
				context.GPR[t] = 0;
				return;
			}
		}

		throw fmt::format("Bad instruction: mrc p%d,%d,r%d,c%d,c%d,%d", cp, opc1, t, cn, cm, opc2);
	}
}

void ARMv7_instrs::ADC_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::ADC_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::ADC_RSR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::ADD_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	bool set_flags = !context.ITSTATE;
	u32 cond, d, n, imm32;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0x7);
		n = (code.data & 0x38) >> 3;
		imm32 = (code.data & 0x1c0) >> 6;
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		d = n = (code.data & 0x700) >> 8;
		imm32 = (code.data & 0xff);
		break;
	}
	case T3:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		n = (code.data & 0xf0000) >> 16;
		set_flags = (code.data & 0x100000);
		imm32 = ThumbExpandImm(context, (code.data & 0x4000000) >> 15 | (code.data & 0x7000) >> 4 | (code.data & 0xff));

		reject(d == 15 && set_flags, "CMN (immediate)");
		reject(n == 13, "ADD (SP plus immediate)");
		reject(d == 13 || n == 15, "UNPREDICTABLE");
		break;
	}
	case T4:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		n = (code.data & 0xf0000) >> 16;
		set_flags = false;
		imm32 = (code.data & 0x4000000) >> 15 | (code.data & 0x7000) >> 4 | (code.data & 0xff);

		reject(n == 15, "ADR");
		reject(n == 13, "ADD (SP plus immediate)");
		reject(d == 13 || d == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		if (set_flags)
		{
			bool carry, overflow;
			const u32 res = AddWithCarry(context.read_gpr(n), imm32, false, carry, overflow);
			context.write_gpr(d, res);
			context.APSR.N = res >> 31;
			context.APSR.Z = res == 0;
			context.APSR.C = carry;
			context.APSR.V = overflow;
		}
		else
		{
			context.write_gpr(d, context.read_gpr(n) + imm32);
		}
	}
}

void ARMv7_instrs::ADD_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	bool set_flags = !context.ITSTATE;
	u32 cond, d, n, m, shift_t, shift_n;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0x7);
		n = (code.data & 0x38) >> 3;
		m = (code.data & 0x1c0) >> 6;
		shift_t = SRType_LSL;
		shift_n = 0;
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		n = d = (code.data & 0x80) >> 4 | (code.data & 0x7);
		m = (code.data & 0x78) >> 3;
		set_flags = false;
		shift_t = SRType_LSL;
		shift_n = 0;

		reject(n == 13 || m == 13, "ADD (SP plus register)");
		reject(n == 15 && m == 15, "UNPREDICTABLE");
		reject(d == 15 && context.ITSTATE, "UNPREDICTABLE");
		break;
	}
	case T3:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		n = (code.data & 0xf0000) >> 16;
		m = (code.data & 0xf);
		set_flags = (code.data & 0x100000);
		shift_t = DecodeImmShift((code.data & 0x30) >> 4, (code.data & 0x7000) >> 10 | (code.data & 0xc0) >> 6, &shift_n);

		reject(d == 15 && set_flags, "CMN (register)");
		reject(n == 13, "ADD (SP plus register)");
		reject(d == 13 || n == 15 || m == 13 || m == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		const u32 shifted = Shift(context.read_gpr(m), shift_t, shift_n, true);
		if (set_flags)
		{
			bool carry, overflow;
			const u32 res = AddWithCarry(context.read_gpr(n), shifted, false, carry, overflow);
			context.write_gpr(d, res);
			context.APSR.N = res >> 31;
			context.APSR.Z = res == 0;
			context.APSR.C = carry;
			context.APSR.V = overflow;
		}
		else
		{
			context.write_gpr(d, context.read_gpr(n) + shifted);
		}
	}
}

void ARMv7_instrs::ADD_RSR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::ADD_SPI(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	bool set_flags;
	u32 cond, d, imm32;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0x700) >> 8;
		set_flags = false;
		imm32 = (code.data & 0xff) << 2;
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		d = 13;
		set_flags = false;
		imm32 = (code.data & 0x7f) << 2;
		break;
	}
	case T3:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		set_flags = (code.data & 0x100000);
		imm32 = ThumbExpandImm(context, (code.data & 0x4000000) >> 15 | (code.data & 0x7000) >> 4 | (code.data & 0xff));

		reject(d == 15 && set_flags, "CMN (immediate)");
		reject(d == 15, "UNPREDICTABLE");
		break;
	}
	case T4:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		set_flags = false;
		imm32 = (code.data & 0x4000000) >> 15 | (code.data & 0x7000) >> 4 | (code.data & 0xff);

		reject(d == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		if (set_flags)
		{
			bool carry, overflow;
			const u32 res = AddWithCarry(context.SP, imm32, false, carry, overflow);
			context.write_gpr(d, res);
			context.APSR.N = res >> 31;
			context.APSR.Z = res == 0;
			context.APSR.C = carry;
			context.APSR.V = overflow;
		}
		else
		{
			context.write_gpr(d, context.SP + imm32);
		}
	}
}

void ARMv7_instrs::ADD_SPR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	bool set_flags;
	u32 cond, d, m, shift_t, shift_n;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		d = m = (code.data & 0x80) >> 4 | (code.data & 0x7);
		set_flags = false;
		shift_t = SRType_LSL;
		shift_n = 0;
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		d = 13;
		m = (code.data & 0x78) >> 3;
		set_flags = false;
		shift_t = SRType_LSL;
		shift_n = 0;

		reject(m == 13, "encoding T1");
		break;
	}
	case T3:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		m = (code.data & 0xf);
		set_flags = (code.data & 0x100000);
		shift_t = DecodeImmShift((code.data & 0x30) >> 4, (code.data & 0x7000) >> 10 | (code.data & 0xc0) >> 6, &shift_n);

		reject(d == 13 && (shift_t != SRType_LSL || shift_n > 3), "UNPREDICTABLE");
		reject(d == 15 || m == 13 || m == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		const u32 shifted = Shift(context.read_gpr(m), shift_t, shift_n, context.APSR.C);
		if (set_flags)
		{
			bool carry, overflow;
			const u32 res = AddWithCarry(context.SP, shifted, false, carry, overflow);
			context.write_gpr(d, res);
			context.APSR.N = res >> 31;
			context.APSR.Z = res == 0;
			context.APSR.C = carry;
			context.APSR.V = overflow;
		}
		else
		{
			context.write_gpr(d, context.SP + context.read_gpr(m));
		}
	}
}


void ARMv7_instrs::ADR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::AND_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::AND_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	bool set_flags = !context.ITSTATE;
	u32 cond, d, n, m, shift_t, shift_n;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		d = n = (code.data & 0x7);
		m = (code.data & 0x38) >> 3;
		shift_t = SRType_LSL;
		shift_n = 0;
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		n = (code.data & 0xf0000) >> 16;
		m = (code.data & 0xf);
		set_flags = (code.data & 0x100000);
		shift_t = DecodeImmShift((code.data & 0x30) >> 4, (code.data & 0x7000) >> 10 | (code.data & 0xc0) >> 6, &shift_n);

		reject(d == 15 && set_flags, "TST (register)");
		reject(d == 13 || d == 15 || n == 13 || n == 15 || m == 13 || m == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		bool carry;
		const u32 shifted = Shift_C(context.read_gpr(m), shift_t, shift_n, context.APSR.C, carry);
		const u32 result = context.read_gpr(n) & shifted;
		context.write_gpr(d, result);

		if (set_flags)
		{
			context.APSR.N = result >> 31;
			context.APSR.Z = result == 0;
			context.APSR.C = carry;
		}
	}
}

void ARMv7_instrs::AND_RSR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::ASR_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::ASR_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::B(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, jump;

	switch (type)
	{
	case T1:
	{
		cond = (code.data >> 8) & 0xf;
		jump = 4 + sign<9, u32>((code.data & 0xff) << 1);

		reject(cond == 14, "UNDEFINED");
		reject(cond == 15, "SVC");
		reject(context.ITSTATE, "UNPREDICTABLE");
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		jump = 4 + sign<12, u32>((code.data & 0x7ff) << 1);

		reject(context.ITSTATE, "UNPREDICTABLE");
		break;
	}
	case T3:
	{
		cond = (code.data >> 22) & 0xf;
		{
			const u32 s = (code.data >> 26) & 0x1;
			const u32 j1 = (code.data >> 13) & 0x1;
			const u32 j2 = (code.data >> 11) & 0x1;
			jump = 4 + sign<21, u32>(s << 20 | j2 << 19 | j1 << 18 | (code.data & 0x3f0000) >> 4 | (code.data & 0x7ff) << 1);
		}

		reject(cond >= 14, "Related encodings");
		reject(context.ITSTATE, "UNPREDICTABLE");
		break;
	}
	case T4:
	{
		cond = context.ITSTATE.advance();
		{
			const u32 s = (code.data >> 26) & 0x1;
			const u32 i1 = (code.data >> 13) & 0x1 ^ s ^ 1;
			const u32 i2 = (code.data >> 11) & 0x1 ^ s ^ 1;
			jump = 4 + sign<25, u32>(s << 24 | i2 << 23 | i1 << 22 | (code.data & 0x3ff0000) >> 4 | (code.data & 0x7ff) << 1);
		}
		
		reject(context.ITSTATE, "UNPREDICTABLE");
		break;
	}
	case A1:
	{
		cond = code.data >> 28;
		jump = 1 + 4 + sign<26, u32>((code.data & 0xffffff) << 2);
		break;
	}
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		context.thread.SetBranch(context.thread.PC + jump);
	}
}


void ARMv7_instrs::BFC(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::BFI(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::BIC_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::BIC_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::BIC_RSR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::BKPT(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::BL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, imm32, newLR;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		newLR = (context.thread.PC + 4) | 1;
		{
			const u32 s = (code.data >> 26) & 0x1;
			const u32 i1 = (code.data >> 13) & 0x1 ^ s ^ 1;
			const u32 i2 = (code.data >> 11) & 0x1 ^ s ^ 1;
			imm32 = 4 + sign<25, u32>(s << 24 | i2 << 23 | i1 << 22 | (code.data & 0x3ff0000) >> 4 | (code.data & 0x7ff) << 1);
		}

		reject(context.ITSTATE, "UNPREDICTABLE");
		break;
	}
	case A1:
	{
		cond = code.data >> 28;
		newLR = (context.thread.PC + 4) - 4;
		imm32 = 4 + sign<26, u32>((code.data & 0xffffff) << 2);
		break;
	}
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		context.LR = newLR;
		context.thread.SetBranch(context.thread.PC + imm32);
	}
}

void ARMv7_instrs::BLX(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, target, newLR;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		newLR = (context.thread.PC + 2) | 1; // ???
		{
			const u32 m = (code.data >> 3) & 0xf;
			reject(m == 15, "UNPREDICTABLE");
			target = context.read_gpr(m);
		}

		reject(context.ITSTATE, "UNPREDICTABLE");
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		newLR = (context.thread.PC + 4) | 1;
		{
			const u32 s = (code.data >> 26) & 0x1;
			const u32 i1 = (code.data >> 13) & 0x1 ^ s ^ 1;
			const u32 i2 = (code.data >> 11) & 0x1 ^ s ^ 1;
			target = (context.thread.PC + 4 & ~3) + sign<25, u32>(s << 24 | i2 << 23 | i1 << 22 | (code.data & 0x3ff0000) >> 4 | (code.data & 0x7ff) << 1);
		}

		reject(context.ITSTATE, "UNPREDICTABLE");
		break;
	}
	case A1:
	{
		cond = code.data >> 28;
		newLR = (context.thread.PC + 4) - 4;
		target = context.read_gpr(code.data & 0xf);
		break;
	}
	case A2:
	{
		cond = 15;
		newLR = (context.thread.PC + 4) - 4;
		target = (context.thread.PC + 4 | 1) + sign<25, u32>((code.data & 0xffffff) << 2 | (code.data & 0x1000000) >> 23);
		break;
	}
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		context.LR = newLR;
		if (target & 1)
		{
			context.ISET = Thumb;
			context.thread.SetBranch(target & ~1);
		}
		else
		{
			context.ISET = ARM;
			context.thread.SetBranch(target);
		}
	}
}

void ARMv7_instrs::BX(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, target;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		target = context.read_gpr((code.data >> 3) & 0xf);

		reject(context.ITSTATE, "UNPREDICTABLE");
		break;
	}
	case A1:
	{
		cond = code.data >> 28;
		target = context.read_gpr(code.data & 0xf);
		break;
	}
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		if (target & 1)
		{
			context.ISET = Thumb;
			context.thread.SetBranch(target & ~1);
		}
		else
		{
			context.ISET = ARM;
			context.thread.SetBranch(target);
		}
	}
}


void ARMv7_instrs::CB_Z(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 n, imm32;
	bool nonzero;

	switch (type)
	{
	case T1:
	{
		n = code.data & 0x7;
		imm32 = (code.data & 0xf8) >> 2 | (code.data & 0x200) >> 3;
		nonzero = (code.data & 0x800);

		reject(context.ITSTATE, "UNPREDICTABLE");
		break;
	}
	default: throw __FUNCTION__;
	}

	if ((context.read_gpr(n) == 0) ^ nonzero)
	{
		context.thread.SetBranch(context.thread.PC + 2 + imm32);
	}
}


void ARMv7_instrs::CLZ(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, d, m;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		m = (code.data & 0xf);

		reject((code.data & 0xf0000) >> 16 != m, "UNPREDICTABLE");
		reject(d == 13 || d == 15 || m == 13 || m == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		context.write_gpr(d, cntlz32(context.read_gpr(m)));
	}
}


void ARMv7_instrs::CMN_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::CMN_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::CMN_RSR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::CMP_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, n, imm32;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		n = (code.data & 0x700) >> 8;
		imm32 = (code.data & 0xff);
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		n = (code.data & 0xf0000) >> 16;
		imm32 = ThumbExpandImm(context, (code.data & 0x4000000) >> 15 | (code.data & 0x7000) >> 4 | (code.data & 0xff));

		reject(n == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		bool carry, overflow;
		const u32 res = AddWithCarry(context.read_gpr(n), ~imm32, true, carry, overflow);
		context.APSR.N = res >> 31;
		context.APSR.Z = res == 0;
		context.APSR.C = carry;
		context.APSR.V = overflow;
	}
}

void ARMv7_instrs::CMP_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, n, m, shift_t, shift_n;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		n = (code.data & 0x7);
		m = (code.data & 0x38) >> 3;
		shift_t = SRType_LSL;
		shift_n = 0;
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		n = (code.data & 0x80) >> 4 | (code.data & 0x7);
		m = (code.data & 0x78) >> 3;
		shift_t = SRType_LSL;
		shift_n = 0;

		reject(n < 8 && m < 8, "UNPREDICTABLE");
		reject(n == 15 || m == 15, "UNPREDICTABLE");
		break;
	}
	case T3:
	{
		n = (code.data & 0xf0000) >> 16;
		m = (code.data & 0xf);
		shift_t = DecodeImmShift((code.data & 0x30) >> 4, (code.data & 0x7000) >> 10 | (code.data & 0xc0) >> 6, &shift_n);

		reject(n == 15 || m == 13 || m == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		bool carry, overflow;
		const u32 shifted = Shift(context.read_gpr(m), shift_t, shift_n, true);
		const u32 res = AddWithCarry(context.read_gpr(n), ~shifted, true, carry, overflow);
		context.APSR.N = res >> 31;
		context.APSR.Z = res == 0;
		context.APSR.C = carry;
		context.APSR.V = overflow;
	}
}

void ARMv7_instrs::CMP_RSR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::EOR_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::EOR_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::EOR_RSR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::IT(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case T1:
	{
		const u32 mask = code.data & 0xf;
		const u32 first = (code.data & 0xf0) >> 4;
		
		reject(mask == 0, "Related encodings");
		reject(first == 15, "UNPREDICTABLE");
		reject(first == 14 && BitCount(mask) != 1, "UNPREDICTABLE");
		reject(context.ITSTATE, "UNPREDICTABLE");
		break;
	}
	default: throw __FUNCTION__;
	}

	context.ITSTATE.IT = code.data & 0xff;
}


void ARMv7_instrs::LDM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::LDMDA(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::LDMDB(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::LDMIB(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::LDR_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, t, n, imm32;
	bool index, add, wback;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		t = (code.data & 0x7);
		n = (code.data & 0x38) >> 3;
		imm32 = (code.data & 0x7c0) >> 4;
		index = true;
		add = true;
		wback = false;
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		t = (code.data & 0x700) >> 8;
		n = 13;
		imm32 = (code.data & 0xff) << 2;
		index = true;
		add = true;
		wback = false;
		break;
	}
	case T3:
	{
		cond = context.ITSTATE.advance();
		t = (code.data & 0xf000) >> 12;
		n = (code.data & 0xf0000) >> 16;
		imm32 = (code.data & 0xfff);
		index = true;
		add = true;
		wback = false;

		reject(n == 15, "LDR (literal)");
		reject(t == 15 && context.ITSTATE, "UNPREDICTABLE");
		break;
	}
	case T4:
	{
		cond = context.ITSTATE.advance();
		t = (code.data & 0xf000) >> 12;
		n = (code.data & 0xf0000) >> 16;
		imm32 = (code.data & 0xff);
		index = (code.data & 0x400);
		add = (code.data & 0x200);
		wback = (code.data & 0x100);

		reject(n == 15, "LDR (literal)");
		reject(index && add && !wback, "LDRT");
		reject(n == 13 && !index && add && wback && imm32 == 4, "POP");
		reject(!index && !wback, "UNDEFINED");
		reject((wback && n == t) || (t == 15 && context.ITSTATE), "UNPREDICTABLE");
		break;
	}
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		const u32 offset_addr = add ? context.read_gpr(n) + imm32 : context.read_gpr(n) - imm32;
		const u32 addr = index ? offset_addr : context.read_gpr(n);

		context.write_gpr(t, vm::psv::read32(addr));

		if (wback)
		{
			context.write_gpr(n, offset_addr);
		}
	}
}

void ARMv7_instrs::LDR_LIT(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::LDR_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, t, n, m, shift_t, shift_n;
	bool index, add, wback;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		t = (code.data & 0x7);
		n = (code.data & 0x38) >> 3;
		m = (code.data & 0x1c0) >> 6;
		index = true;
		add = true;
		wback = false;
		shift_t = SRType_LSL;
		shift_n = 0;
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		t = (code.data & 0xf000) >> 12;
		n = (code.data & 0xf0000) >> 16;
		m = (code.data & 0xf);
		index = true;
		add = true;
		wback = false;
		shift_t = (code.data & 0x30) >> 4;
		shift_n = 0;

		reject(n == 15, "LDR (literal)");
		reject(m == 13 || m == 15, "UNPREDICTABLE");
		reject(t == 15 && context.ITSTATE, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		const u32 offset = Shift(context.read_gpr(m), shift_t, shift_n, context.APSR.C);
		const u32 offset_addr = add ? context.read_gpr(n) + offset : context.read_gpr(n) - offset;
		const u32 addr = index ? offset_addr : context.read_gpr(n);
		const u32 data = vm::psv::read32(addr);

		if (wback)
		{
			context.write_gpr(n, offset_addr);
		}

		context.write_gpr(t, data);
	}
}


void ARMv7_instrs::LDRB_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::LDRB_LIT(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::LDRB_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::LDRD_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, t, t2, n, imm32;
	bool index, add, wback;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		t = (code.data & 0xf000) >> 12;
		t2 = (code.data & 0xf00) >> 8;
		n = (code.data & 0xf0000) >> 16;
		imm32 = (code.data & 0xff) << 2;
		index = (code.data & 0x1000000);
		add = (code.data & 0x800000);
		wback = (code.data & 0x200000);

		reject(!index && !wback, "Related encodings");
		reject(n == 15, "LDRD (literal)");
		reject(wback && (n == t || n == t2), "UNPREDICTABLE");
		reject(t == 13 || t == 15 || t2 == 13 || t2 == 15 || t == t2, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		const u32 offset_addr = add ? context.read_gpr(n) + imm32 : context.read_gpr(n) - imm32;
		const u32 addr = index ? offset_addr : context.read_gpr(n);
		const u64 value = vm::psv::read64(addr);

		context.write_gpr(t, (u32)(value));
		context.write_gpr(t2, (u32)(value >> 32));

		if (wback)
		{
			context.write_gpr(n, offset_addr);
		}
	}
}

void ARMv7_instrs::LDRD_LIT(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::LDRD_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::LDRH_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::LDRH_LIT(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::LDRH_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::LDRSB_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::LDRSB_LIT(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::LDRSB_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::LDRSH_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::LDRSH_LIT(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::LDRSH_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::LDREX(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, t, n, imm32;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		t = (code.data & 0xf000) >> 12;
		n = (code.data & 0xf0000) >> 16;
		imm32 = (code.data & 0xff) << 2;

		reject(t == 13 || t == 15 || n == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		const u32 addr = context.read_gpr(n) + imm32;
		const u32 value = vm::psv::read32(addr);
		
		context.R_ADDR = addr;
		context.R_DATA = value;
		context.write_gpr(t, value);
	}
}

void ARMv7_instrs::LDREXB(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::LDREXD(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::LDREXH(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::LSL_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	bool set_flags = !context.ITSTATE;
	u32 cond, d, m, shift_n;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0x7);
		m = (code.data & 0x38) >> 3;
		shift_n = (code.data & 0x7c0) >> 6;

		reject(!shift_n, "MOV (register)");
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		m = (code.data & 0xf);
		set_flags = (code.data & 0x100000);
		shift_n = (code.data & 0x7000) >> 10 | (code.data & 0xc0) >> 6;

		reject(!shift_n, "MOV (register)");
		reject(d == 13 || d == 15 || m == 13 || m == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		bool carry;
		const u32 res = Shift_C(context.read_gpr(m), SRType_LSL, shift_n, context.APSR.C, carry);
		context.write_gpr(d, res);
		if (set_flags)
		{
			context.APSR.N = res >> 31;
			context.APSR.Z = res == 0;
			context.APSR.C = carry;
		}
	}
}

void ARMv7_instrs::LSL_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	bool set_flags = !context.ITSTATE;
	u32 cond, d, n, m;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		d = n = (code.data & 0x7);
		m = (code.data & 0x38) >> 3;
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		n = (code.data & 0xf0000) >> 16;
		m = (code.data & 0xf);
		set_flags = (code.data & 0x100000);

		reject(d == 13 || d == 15 || n == 13 || n == 15 || m == 13 || m == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		bool carry;
		const u32 res = Shift_C(context.read_gpr(n), SRType_LSL, (context.read_gpr(m) & 0xff), context.APSR.C, carry);
		context.write_gpr(d, res);
		if (set_flags)
		{
			context.APSR.N = res >> 31;
			context.APSR.Z = res == 0;
			context.APSR.C = carry;
		}
	}
}


void ARMv7_instrs::LSR_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::LSR_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::MLA(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::MLS(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::MOV_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	bool set_flags = !context.ITSTATE;
	bool carry = context.APSR.C;
	u32 cond, d, imm32;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		d = (code.data >> 8) & 0x7;
		imm32 = sign<8, u32>(code.data & 0xff);
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		set_flags = code.data & 0x100000;
		d = (code.data >> 8) & 0xf;
		imm32 = ThumbExpandImm_C((code.data & 0x4000000) >> 15 | (code.data & 0x7000) >> 4 | (code.data & 0xff), carry, carry);

		reject(d == 13 || d == 15, "UNPREDICTABLE");
		break;
	}
	case T3:
	{
		cond = context.ITSTATE.advance();
		set_flags = false;
		d = (code.data >> 8) & 0xf;
		imm32 = (code.data & 0xf0000) >> 4 | (code.data & 0x4000000) >> 15 | (code.data & 0x7000) >> 4 | (code.data & 0xff);

		reject(d == 13 || d == 15, "UNPREDICTABLE");
		break;
	}
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		context.write_gpr(d, imm32);
		if (set_flags)
		{
			context.APSR.N = imm32 >> 31;
			context.APSR.Z = imm32 == 0;
			context.APSR.C = carry;
		}
	}
}

void ARMv7_instrs::MOV_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, d, m;
	bool set_flags;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0x80) >> 4 | (code.data & 0x7);
		m = (code.data & 0x78) >> 3;
		set_flags = false;

		reject(d == 15 && context.ITSTATE, "UNPREDICTABLE");
		break;
	}
	case T2:
	{
		cond = 15;
		d = (code.data & 0x7);
		m = (code.data & 0x38) >> 3;
		set_flags = true;

		reject(context.ITSTATE, "UNPREDICTABLE");
		break;
	}
	case T3:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		m = (code.data & 0xf);
		set_flags = (code.data & 0x100000);

		reject((d == 13 || m == 13 || m == 15) && set_flags, "UNPREDICTABLE");
		reject((d == 13 && (m == 13 || m == 15)) || d == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		const u32 res = context.read_gpr(m);
		context.write_gpr(d, res);
		if (set_flags)
		{
			context.APSR.N = res >> 31;
			context.APSR.Z = res == 0;
			//context.APSR.C = ?
		}
	}
}

void ARMv7_instrs::MOVT(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, d, imm16;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		imm16 = (code.data & 0xf0000) >> 4 | (code.data & 0x4000000) >> 14 | (code.data & 0x7000) >> 4 | (code.data & 0xff);

		reject(d == 13 || d == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		context.write_gpr(d, (context.read_gpr(d) & 0xffff) | (imm16 << 16));
	}
}


void ARMv7_instrs::MRS(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::MSR_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::MSR_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::MUL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	bool set_flags = !context.ITSTATE;
	u32 cond, d, n, m;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		d = m = code.data & 0x7;
		n = (code.data & 0x38) >> 3;

		//reject(ArchVersion() < 6 && d == n, "UNPREDICTABLE");
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		n = (code.data & 0xf0000) >> 16;
		m = (code.data & 0xf);
		set_flags = false;

		reject(d == 13 || d == 15 || n == 13 || n == 15 || m == 13 || m == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		const u32 op1 = context.read_gpr(n);
		const u32 op2 = context.read_gpr(m);
		const u32 result = op1 * op2;
		context.write_gpr(d, result);

		if (set_flags)
		{
			context.APSR.N = result >> 31;
			context.APSR.Z = result == 0;
		}
	}
}


void ARMv7_instrs::MVN_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, d, imm32;
	bool set_flags, carry;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		set_flags = (code.data & 0x100000);
		imm32 = ThumbExpandImm_C((code.data & 0x4000000) >> 15 | (code.data & 0x7000) >> 4 | (code.data & 0xff), context.APSR.C, carry);

		reject(d == 13 || d == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		const u32 result = ~imm32;
		context.write_gpr(d, result);

		if (set_flags)
		{
			context.APSR.N = result >> 31;
			context.APSR.Z = result == 0;
			context.APSR.C = carry;
		}
	}
}

void ARMv7_instrs::MVN_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::MVN_RSR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::NOP(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond; 

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		break;
	}
	case A1:
	{
		cond = code.data >> 28;
		break;
	}
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
	}
}


void ARMv7_instrs::ORN_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::ORN_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::ORR_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::ORR_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::ORR_RSR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::PKH(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::POP(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, reg_list;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		reg_list = ((code.data & 0x100) << 7) | (code.data & 0xff);

		reject(!reg_list, "UNPREDICTABLE");
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		reg_list = code.data & 0xdfff;

		reject(BitCount(reg_list, 16) < 2 || ((reg_list & 0x8000) && (reg_list & 0x4000)), "UNPREDICTABLE");
		reject((reg_list & 0x8000) && context.ITSTATE, "UNPREDICTABLE");
		break;
	}
	case T3:
	{
		cond = context.ITSTATE.advance();
		reg_list = 1 << ((code.data & 0xf000) >> 12);

		reject((reg_list & 0x2000) || ((reg_list & 0x8000) && context.ITSTATE), "UNPREDICTABLE");
		break;
	}
	case A1:
	{
		cond = code.data >> 28;
		reg_list = code.data & 0xffff;

		reject(BitCount(reg_list, 16) < 2, "LDM / LDMIA / LDMFD");
		reject((reg_list & 0x2000) /* && ArchVersion() >= 7*/, "UNPREDICTABLE");
		break;
	}
	case A2:
	{
		cond = code.data >> 28;
		reg_list = 1 << ((code.data & 0xf000) >> 12);

		reject(reg_list & 0x2000, "UNPREDICTABLE");
		break;
	}
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		u32 written = 0;
		for (u16 mask = 1, i = 0; mask; mask <<= 1, i++)
		{
			if (reg_list & mask)
			{
				context.write_gpr(i, vm::psv::read32(context.SP + written));
				written += 4;
			}
		}

		context.SP += written;
	}
}

void ARMv7_instrs::PUSH(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, reg_list;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		reg_list = ((code.data & 0x100) << 6) | (code.data & 0xff);

		reject(!reg_list, "UNPREDICTABLE");
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		reg_list = code.data & 0x5fff;

		reject(BitCount(reg_list, 16) < 2, "UNPREDICTABLE");
		break;
	}
	case T3:
	{
		cond = context.ITSTATE.advance();
		reg_list = 1 << ((code.data & 0xf000) >> 12);

		reject((reg_list & 0x8000) || (reg_list & 0x2000), "UNPREDICTABLE");
		break;
	}
	case A1:
	{
		cond = code.data >> 28;
		reg_list = code.data & 0xffff;

		reject(BitCount(reg_list) < 2, "STMDB / STMFD");
		break;
	}
	case A2:
	{
		cond = code.data >> 28;
		reg_list = 1 << ((code.data & 0xf000) >> 12);

		reject(reg_list & 0x2000, "UNPREDICTABLE");
		break;
	}
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		u32 read = 0;
		for (u16 mask = 1 << 15, i = 15; mask; mask >>= 1, i--)
		{
			if (reg_list & mask)
			{
				read += 4;
				vm::psv::write32(context.SP - read, context.read_gpr(i));
			}
		}

		context.SP -= read;
	}
}


void ARMv7_instrs::QADD(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::QADD16(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::QADD8(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::QASX(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::QDADD(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::QDSUB(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::QSAX(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::QSUB(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::QSUB16(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::QSUB8(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::RBIT(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::REV(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::REV16(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::REVSH(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::ROR_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::ROR_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::RRX(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::RSB_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::RSB_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::RSB_RSR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::RSC_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::RSC_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::RSC_RSR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::SADD16(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SADD8(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SASX(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::SBC_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SBC_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SBC_RSR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::SBFX(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::SDIV(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::SEL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::SHADD16(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SHADD8(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SHASX(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SHSAX(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SHSUB16(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SHSUB8(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::SMLA__(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SMLAD(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SMLAL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SMLAL__(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SMLALD(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SMLAW_(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SMLSD(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SMLSLD(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SMMLA(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SMMLS(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SMMUL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SMUAD(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SMUL__(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SMULL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SMULW_(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SMUSD(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::SSAT(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SSAT16(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SSAX(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SSUB16(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SSUB8(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::STM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::STMDA(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::STMDB(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::STMIB(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::STR_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, t, n, imm32;
	bool index, add, wback;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		t = (code.data & 0x7);
		n = (code.data & 0x38) >> 3;
		imm32 = (code.data & 0x7c0) >> 4;
		index = true;
		add = true;
		wback = false;
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		t = (code.data & 0x700) >> 8;
		n = 13;
		imm32 = (code.data & 0xff) << 2;
		index = true;
		add = true;
		wback = false;
		break;
	}
	case T3:
	{
		cond = context.ITSTATE.advance();
		t = (code.data & 0xf000) >> 12;
		n = (code.data & 0xf0000) >> 16;
		imm32 = (code.data & 0xfff);
		index = true;
		add = true;
		wback = false;

		reject(n == 15, "UNDEFINED");
		reject(t == 15, "UNPREDICTABLE");
		break;
	}
	case T4:
	{
		cond = context.ITSTATE.advance();
		t = (code.data & 0xf000) >> 12;
		n = (code.data & 0xf0000) >> 16;
		imm32 = (code.data & 0xff);
		index = (code.data & 0x400);
		add = (code.data & 0x200);
		wback = (code.data & 0x100);

		reject(index && add && !wback, "STRT");
		reject(n == 13 && index && !add && wback && imm32 == 4, "PUSH");
		reject(n == 15 || (!index && !wback), "UNDEFINED");
		reject(t == 15 || (wback && n == t), "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		const u32 offset_addr = add ? context.read_gpr(n) + imm32 : context.read_gpr(n) - imm32;
		const u32 addr = index ? offset_addr : context.read_gpr(n);

		vm::psv::write32(addr, context.read_gpr(t));

		if (wback)
		{
			context.write_gpr(n, offset_addr);
		}
	}
}

void ARMv7_instrs::STR_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, t, n, m, shift_t, shift_n;
	bool index, add, wback;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		t = (code.data & 0x7);
		n = (code.data & 0x38) >> 3;
		m = (code.data & 0x1c0) >> 6;
		index = true;
		add = true;
		wback = false;
		shift_t = SRType_LSL;
		shift_n = 0;
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		t = (code.data & 0xf000) >> 12;
		n = (code.data & 0xf0000) >> 16;
		m = (code.data & 0xf);
		index = true;
		add = true;
		wback = false;
		shift_t = SRType_LSL;
		shift_n = (code.data & 0x30) >> 4;

		reject(n == 15, "UNDEFINED");
		reject(t == 15 || m == 13 || m == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		const u32 offset = Shift(context.read_gpr(m), shift_t, shift_n, context.APSR.C);
		const u32 offset_addr = add ? context.read_gpr(n) + offset : context.read_gpr(n) - offset;
		const u32 addr = index ? offset_addr : context.read_gpr(n);

		vm::psv::write32(addr, context.read_gpr(t));

		if (wback)
		{
			context.write_gpr(n, offset_addr);
		}
	}
}


void ARMv7_instrs::STRB_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::STRB_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::STRD_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::STRD_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::STRH_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::STRH_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::STREX(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, d, t, n, imm32;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		t = (code.data & 0xf000) >> 12;
		n = (code.data & 0xf0000) >> 16;
		imm32 = (code.data & 0xff) << 2;

		reject(d == 13 || d == 15 || t == 13 || t == 15 || n == 15, "UNPREDICTABLE");
		reject(d == n || d == t, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		const u32 addr = context.read_gpr(n) + imm32;
		const u32 value = context.read_gpr(t);
		
		auto& sync_obj = vm::get_ref<atomic_le_t<u32>>(addr);
		context.write_gpr(d, addr != context.R_ADDR || sync_obj.compare_and_swap((u32)context.R_DATA, value) != context.R_DATA);
		context.R_ADDR = 0;
	}
}

void ARMv7_instrs::STREXB(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::STREXD(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::STREXH(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::SUB_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	bool set_flags = !context.ITSTATE;
	u32 cond, d, n, imm32;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0x7);
		n = (code.data & 0x38) >> 3;
		imm32 = (code.data & 0x1c) >> 6;
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		d = n = (code.data & 0x700) >> 8;
		imm32 = (code.data & 0xff);
		break;
	}
	case T3:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		n = (code.data & 0xf0000) >> 16;
		set_flags = (code.data & 0x100000);
		imm32 = ThumbExpandImm(context, (code.data & 0x4000000) >> 15 | (code.data & 0x7000) >> 4 | (code.data & 0xff));

		reject(d == 15 && set_flags, "CMP (immediate)");
		reject(n == 13, "SUB (SP minus immediate)");
		reject(d == 13 || d == 15 || n == 15, "UNPREDICTABLE");
		break;
	}
	case T4:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		n = (code.data & 0xf0000) >> 16;
		set_flags = false;
		imm32 = (code.data & 0x4000000) >> 15 | (code.data & 0x7000) >> 4 | (code.data & 0xff);

		reject(d == 15, "ADR");
		reject(n == 13, "SUB (SP minus immediate)");
		reject(d == 13 || d == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		if (set_flags)
		{
			bool carry, overflow;
			const u32 res = AddWithCarry(context.read_gpr(n), ~imm32, true, carry, overflow);
			context.write_gpr(d, res);
			context.APSR.N = res >> 31;
			context.APSR.Z = res == 0;
			context.APSR.C = carry;
			context.APSR.V = overflow;
		}
		else
		{
			context.write_gpr(d, context.read_gpr(n) - imm32);
		}
	}
}

void ARMv7_instrs::SUB_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	bool set_flags = !context.ITSTATE;
	u32 cond, d, n, m, shift_t, shift_n;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0x7);
		n = (code.data & 0x38) >> 3;
		m = (code.data & 0x1c0) >> 6;
		shift_t = SRType_LSL;
		shift_n = 0;
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		n = (code.data & 0xf0000) >> 16;
		m = (code.data & 0xf);
		set_flags = (code.data & 0x100000);
		shift_t = DecodeImmShift((code.data & 0x30) >> 4, (code.data & 0x7000) >> 10 | (code.data & 0xc0) >> 6, &shift_n);

		reject(d == 15 && set_flags, "CMP (register)");
		reject(n == 13, "SUB (SP minus register)");
		reject(d == 13 || d == 15 || n == 15 || m == 13 || m == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		const u32 shifted = Shift(context.read_gpr(m), shift_t, shift_n, context.APSR.C);
		if (set_flags)
		{
			bool carry, overflow;
			const u32 res = AddWithCarry(context.read_gpr(n), ~shifted, true, carry, overflow);
			context.write_gpr(d, res);
			context.APSR.N = res >> 31;
			context.APSR.Z = res == 0;
			context.APSR.C = carry;
			context.APSR.V = overflow;
		}
		else
		{
			context.write_gpr(d, context.read_gpr(n) - shifted);
		}
	}
}

void ARMv7_instrs::SUB_RSR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SUB_SPI(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, d, imm32;
	bool set_flags;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		d = 13;
		set_flags = false;
		imm32 = (code.data & 0x7f) << 2;
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		set_flags = (code.data & 0x100000);
		imm32 = ThumbExpandImm(context, (code.data & 0x4000000) >> 15 | (code.data & 0x7000) >> 4 | (code.data & 0xff));

		reject(d == 15 && set_flags, "CMP (immediate)");
		reject(d == 15, "UNPREDICTABLE");
		break;
	}
	case T3:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		set_flags = false;
		imm32 = (code.data & 0x4000000) >> 15 | (code.data & 0x7000) >> 4 | (code.data & 0xff);

		reject(d == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		if (set_flags)
		{
			bool carry, overflow;
			const u32 res = AddWithCarry(context.SP, ~imm32, true, carry, overflow);
			context.write_gpr(d, res);
			context.APSR.N = res >> 31;
			context.APSR.Z = res == 0;
			context.APSR.C = carry;
			context.APSR.V = overflow;
		}
		else
		{
			context.write_gpr(d, context.SP - imm32);
		}
	}
}

void ARMv7_instrs::SUB_SPR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::SVC(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::SXTAB(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SXTAB16(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SXTAH(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SXTB(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SXTB16(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::SXTH(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::TB_(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::TEQ_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::TEQ_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::TEQ_RSR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::TST_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::TST_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::TST_RSR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::UADD16(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UADD8(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UASX(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UBFX(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UDIV(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UHADD16(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UHADD8(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UHASX(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UHSAX(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UHSUB16(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UHSUB8(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UMAAL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UMLAL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UMULL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UQADD16(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UQADD8(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UQASX(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UQSAX(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UQSUB16(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UQSUB8(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::USAD8(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::USADA8(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::USAT(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::USAT16(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::USAX(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::USUB16(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::USUB8(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UXTAB(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UXTAB16(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UXTAH(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UXTB(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	u32 cond, d, m, rot;

	switch (type)
	{
	case T1:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0x7);
		m = (code.data & 0x38) >> 3;
		rot = 0;
		break;
	}
	case T2:
	{
		cond = context.ITSTATE.advance();
		d = (code.data & 0xf00) >> 8;
		m = (code.data & 0xf);
		rot = (code.data & 0x30) >> 1;

		reject(d == 13 || d == 15 || m == 13 || m == 15, "UNPREDICTABLE");
		break;
	}
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}

	if (ConditionPassed(context, cond))
	{
		context.write_gpr(d, (context.read_gpr(m) >> rot) & 0xff);
	}
}

void ARMv7_instrs::UXTB16(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::UXTH(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::VABA_(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VABD_(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VABD_FP(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VABS(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VAC__(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VADD(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VADD_FP(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VADDHN(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VADD_(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VAND(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VBIC_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VBIC_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VB__(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VCEQ_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VCEQ_ZERO(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VCGE_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VCGE_ZERO(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VCGT_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VCGT_ZERO(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VCLE_ZERO(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VCLS(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VCLT_ZERO(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VCLZ(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VCMP_(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VCNT(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VCVT_FIA(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VCVT_FIF(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VCVT_FFA(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VCVT_FFF(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VCVT_DF(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VCVT_HFA(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VCVT_HFF(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VDIV(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VDUP_S(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VDUP_R(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VEOR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VEXT(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VHADDSUB(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VLD__MS(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VLD1_SL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VLD1_SAL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VLD2_SL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VLD2_SAL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VLD3_SL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VLD3_SAL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VLD4_SL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VLD4_SAL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VLDM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VLDR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VMAXMIN(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VMAXMIN_FP(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VML__(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VML__FP(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VML__S(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VMOV_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VMOV_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VMOV_RS(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VMOV_SR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VMOV_RF(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VMOV_2RF(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VMOV_2RD(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VMOVL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VMOVN(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VMRS(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VMSR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VMUL_(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VMUL_FP(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VMUL_S(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VMVN_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VMVN_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VNEG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VNM__(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VORN_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VORR_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VORR_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VPADAL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VPADD(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VPADD_FP(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VPADDL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VPMAXMIN(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VPMAXMIN_FP(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VPOP(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VPUSH(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VQABS(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VQADD(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VQDML_L(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VQDMULH(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VQDMULL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VQMOV_N(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VQNEG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VQRDMULH(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VQRSHL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VQRSHR_N(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VQSHL_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VQSHL_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VQSHR_N(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VQSUB(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VRADDHN(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VRECPE(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VRECPS(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VREV__(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VRHADD(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VRSHL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VRSHR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VRSHRN(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VRSQRTE(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VRSQRTS(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VRSRA(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VRSUBHN(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VSHL_IMM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VSHL_REG(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VSHLL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VSHR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VSHRN(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VSLI(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VSQRT(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VSRA(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VSRI(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VST__MS(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VST1_SL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VST2_SL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VST3_SL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VST4_SL(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VSTM(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VSTR(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VSUB(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VSUB_FP(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VSUBHN(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VSUB_(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VSWP(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VTB_(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VTRN(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VTST(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VUZP(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::VZIP(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}


void ARMv7_instrs::WFE(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::WFI(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}

void ARMv7_instrs::YIELD(ARMv7Context& context, const ARMv7Code code, const ARMv7_encoding type)
{
	switch (type)
	{
	case A1: throw __FUNCTION__;
	default: throw __FUNCTION__;
	}
}
