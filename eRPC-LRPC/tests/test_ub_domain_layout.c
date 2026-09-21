#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <lrpc/ub_domain_layout.h>
int main(void)
{
    struct ud_contract c;
    __u64 tables[4][512];
    unsigned int d, l, i;
    memset(&c, 0, sizeof(c));
    c.magic = UD_MAGIC; c.abi = UD_ABI; c.nonce = 1;
    c.wb_index = 4; c.nc_index = 3; c.heap_pa = 0x12340000;
    for (d = 0; d < UD_DOMAINS; ++d) {
        /* Deliberately discontinuous remote table pages. */
        for (l = 0; l < 4; ++l) c.table_pa[d][l] = 0x100000 + d*0x100000 + l*0x2000;
        c.code_pa[d] = 0x800000 + d*0x10000;
        c.stack_pa[d] = c.code_pa[d] + 0x1000;
        c.args_pa[d] = c.code_pa[d] + 0x2000;
    }
    assert(ud_contract_valid(&c));
    for (d = 0; d < UD_DOMAINS; ++d) {
        for (l = 0; l < 4; ++l) ud_build_table(&c, d, l, tables[l]);
        for (l = 0; l < 3; ++l) {
            unsigned int slot = (UD_VA >> (39 - l*9)) & 511;
            assert((tables[l][slot] & UD_PA_MASK) == c.table_pa[d][l+1]);
            for (i = 0; i < 512; ++i) if (i != slot) assert(!tables[l][i]);
        }
        assert((tables[3][0] & UD_PA_MASK) == c.code_pa[d]);
        assert(tables[3][0] & (1ULL << 7));
        assert(!(tables[3][0] & UD_UXN));
        assert(((tables[3][0] >> 2) & 7) == c.wb_index);
        assert((tables[3][4] & UD_PA_MASK) == c.heap_pa);
        assert(tables[3][4] & UD_UXN);
        assert(tables[3][4] & (1ULL << 7));
        assert(((tables[3][4] >> 2) & 7) == c.nc_index);
        for (i = 0; i < 512; ++i)
            if (i != 0 && i != 2 && i != 4 && i != 6) assert(!tables[3][i]);
    }
    c.table_pa[0][0] |= 1; assert(!ud_contract_valid(&c));
    c.table_pa[0][0] &= ~1ULL;
    c.heap_pa = 1ULL << 48; assert(!ud_contract_valid(&c));
    puts("UB_DOMAIN_LAYOUT_PASS hardware_execution_tested=0");
    return 0;
}
