#!/usr/bin/env python3
"""Final ELF audit parser regressions: ROM, linked code and absent symbols."""
import unittest
from analyze_native_agc_phy import elf_bindings


class ElfBindingTest(unittest.TestCase):
    def test_rom_and_linked_code(self):
        symbols = """
40001680 g       *ABS*  00000000 bb_agc_reg_update
40800100 g     F .iram0.text  00000104 phy_agc_reg_init_new
00000000         *UND*  00000000 phy_set_bb_wdg
"""
        disassembly = """
40801000 <phy_reg_init_new>:
40801000: 008000ef jal 40800100 <phy_agc_reg_init_new>
40801004: 680080e7 jalr -1920(ra) # 40001680 <bb_agc_reg_update>
40801008: 00000013 nop # 40001680 <bb_agc_reg_update>
4080100c: 0000006f j 40800104 <phy_agc_reg_init_new+0x4>
"""
        result = elf_bindings(symbols, disassembly)
        self.assertEqual(result["bb_agc_reg_update"]["kind"], "rom_address")
        self.assertEqual(result["phy_agc_reg_init_new"]["kind"], "linked_code")
        self.assertEqual(result["phy_set_bb_wdg"]["kind"], "undefined")
        self.assertEqual(result["phy_rx_pkdet_num_set"]["kind"], "not_in_symbol_table")
        self.assertEqual(len(result["bb_agc_reg_update"]["call_sites"]), 1)
        self.assertEqual(len(result["phy_agc_reg_init_new"]["call_sites"]), 1)
        self.assertEqual(result["bb_agc_reg_update"]["call_sites"][0]["caller"],
                         "phy_reg_init_new")


if __name__ == "__main__":
    unittest.main()
