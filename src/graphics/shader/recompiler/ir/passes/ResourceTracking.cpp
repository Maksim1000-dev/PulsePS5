	bool GetHandle(Value value, ValueOpcode expected, uint32_t width, uint32_t pc,
	               uint32_t base_reg, Inst*& handle, uint32_t& source, bool sampler = false,
	               bool sample_adjust = false) {
		handle = value.Resolve().TryInstruction();
		if (handle == nullptr || handle->GetOpcode() != expected) {
			Fail(pc, fmt::format("memory operation requires {}", ValueOpcodeName(expected)));
		}
		DescriptorSource descriptor;
		MakeSource(*handle, width, sampler, sample_adjust, base_reg, descriptor, pc);
		uint32_t bad_dword = 0;
		if (!ValidateSource(descriptor, bad_dword)) {
			if (expected == ValueOpcode::GetBufferResource &&
			    std::all_of(descriptor.dwords.begin(), descriptor.dwords.begin() + width,
			                [](Value word) { return word.Resolve().GetType() == Type::U32; })) {
				return false;
			}
			if (expected == ValueOpcode::GetSamplerResource && bad_dword == 3u) {
				std::printf("Warning: shader resource tracking fallback: hash=0x%016llx stage=%s "
				            "pc=0x%08x GetSamplerResource dword 3 is not a valid runtime value; "
				            "using 0u fallback descriptor\n",
				            static_cast<unsigned long long>(m_program.shader_hash),
				            StageName(m_program.stage), pc);
				descriptor.dwords[3] = Value(0u);
				if (!ValidateSource(descriptor, bad_dword)) {
					Fail(pc, fmt::format("{} dword {} is not a valid runtime value after fallback",
					                     ValueOpcodeName(expected), bad_dword));
				}
				source = InternSource(descriptor);
				return true;
			}
			Fail(pc, fmt::format("{} dword {} is not a valid runtime value",
			                     ValueOpcodeName(expected), bad_dword));
		}
		source = InternSource(descriptor);
		return true;
	}
