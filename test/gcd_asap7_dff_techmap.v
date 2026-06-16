// Techmap rule: rewrite the single ASAP7 FF cell type used in
// gcd_asap7.v (DFFHQNx1_ASAP7_75t_R) into yosys's generic $_DFF_P_
// primitive so the AIGER backend emits it as a sequential latch.
//
// DFFHQNx1_ASAP7_75t_R: positive-edge non-inverting flop.
// Liberty: next_state = "!D"; QN function = "IQN" (=!IQ);
// so on the rising clock edge, QN = !(!D) = D.

(* techmap_celltype = "DFFHQNx1_ASAP7_75t_R" *)
module DFFHQNx1_ASAP7_75t_R_techmap (D, CLK, QN);
  input D, CLK;
  output QN;
  \$_DFF_P_ _TECHMAP_REPLACE_ (.D(D), .C(CLK), .Q(QN));
endmodule
