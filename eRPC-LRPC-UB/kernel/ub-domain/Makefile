obj-m += ub_lrpc_domain.o
ub_lrpc_domain-y := domain.o gate.o gate_nested.o services.o social_services.o
ifeq ($(UD_BUILD_VHE),1)
obj-m += ub_lrpc_domain_vhe.o
ub_lrpc_domain_vhe-y := domain_vhe.o gate_vhe.o gate_nested_vhe.o services.o social_services.o
endif
ccflags-y += -I$(src)/../../include -I$(src)/../../include/uapi
ifeq ($(UD_TRACE),1)
ccflags-y += -DUD_TRACE_ENABLE
endif
