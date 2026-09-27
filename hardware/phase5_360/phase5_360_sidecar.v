// C5 Golden passthrough with a delayed, exact Phase5 winding override.
// Source-level synchronous reference ONLY: this assumes IQ and Golden DAC
// are safely sampled by the same clk40. A real board needs clock-domain,
// alignment, mode and output-switch qualification before assertion of
// alignment_valid. The default 8192-pair delay is nominal, not measured.
module phase5_360_sidecar #(
    parameter integer DELAY_PAIRS = 8192,
    parameter integer ADDR_BITS = $clog2(DELAY_PAIRS)
) (
    input  wire       clk40,
    input  wire       reset,
    input  wire [7:0] raw_iq,
    input  wire [5:0] golden_in,
    input  wire       alignment_valid,
    output wire [5:0] dac_out
);
    localparam [1:0] PASS = 2'b00, FORCE_LOW = 2'b01, FORCE_HIGH = 2'b10;
    reg [4:0] phase_rom [0:255];
    initial $readmemh("hardware/phase5_360/phase5_256x5.mem", phase_rom);
    // Only used as a lock cross-check; the C5 still produces the DAC code.
    reg [5:0] golden_rom [0:2047];
    initial $readmemh("hardware/phase5_360/dac_2048x6.mem", golden_rom);

    reg first_sample;
    reg previous_valid;
    reg [4:0] previous_phase, middle_phase;
    wire [4:0] current_phase = phase_rom[raw_iq];
    wire [4:0] u = middle_phase - previous_phase;
    wire [4:0] e = current_phase - previous_phase;
    wire negative_winding = !e[4] && u[4] &&
                            ({1'b0, u} <= (6'd16 + {1'b0, e}));
    wire positive_winding = e[4] && !u[4] &&
                            (u > (e - 5'd16));
    wire [1:0] new_action = !previous_valid ? PASS :
                            positive_winding ? FORCE_HIGH :
                            negative_winding ? FORCE_LOW : PASS;
    wire [5:0] predicted_golden = golden_rom[{1'b0, previous_phase,
                                             current_phase}];

    // One action per 50 ns. Reading before writing this slot yields the
    // correction from DELAY_PAIRS earlier input pairs. A physical lock must
    // determine the actual latency, pair parity and clock relationship.
    reg [1:0] action_fifo [0:DELAY_PAIRS-1];
    reg [5:0] expected_fifo [0:DELAY_PAIRS-1];
    reg [ADDR_BITS-1:0] write_addr;
    reg [ADDR_BITS:0] filled;
    reg [1:0] delayed_action;
    reg [5:0] delayed_expected;
    reg delayed_valid;
    wire ready = (filled == DELAY_PAIRS);

    always @(posedge clk40) begin
        if (reset) begin
            first_sample <= 1'b1;
            previous_valid <= 1'b0;
            previous_phase <= 5'd0;
            middle_phase <= 5'd0;
            write_addr <= {ADDR_BITS{1'b0}};
            filled <= {(ADDR_BITS+1){1'b0}};
            delayed_action <= PASS;
            delayed_expected <= 6'd20;
            delayed_valid <= 1'b0;
        end else if (first_sample) begin
            middle_phase <= current_phase;
            first_sample <= 1'b0;
        end else begin
            delayed_action <= action_fifo[write_addr];
            delayed_expected <= expected_fifo[write_addr];
            delayed_valid <= ready;
            action_fifo[write_addr] <= new_action;
            expected_fifo[write_addr] <= previous_valid ? predicted_golden : 6'd20;
            if (write_addr == DELAY_PAIRS - 1)
                write_addr <= {ADDR_BITS{1'b0}};
            else
                write_addr <= write_addr + 1'b1;
            if (!ready)
                filled <= filled + 1'b1;
            previous_phase <= current_phase;
            previous_valid <= 1'b1;
            first_sample <= 1'b1;
        end
    end

    // A mismatch always passes Golden. A matching sample alone does not prove
    // alignment: alignment_valid must be earned by a unique multi-pair lock.
    assign dac_out = (!alignment_valid || !delayed_valid ||
                      golden_in != delayed_expected) ? golden_in :
                     delayed_action == FORCE_LOW ? 6'd0 :
                     delayed_action == FORCE_HIGH ? 6'd63 : golden_in;
endmodule
