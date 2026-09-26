// Reference pipeline for an external 40 MHz logic device.
// Inputs: packed Q4/I4, sampled on every clk40 edge (middle, current, ...).
// Output: one 6-bit CVBS DAC code per 50 ns, held across two clk40 periods.
// The first pair after reset is priming and emits pedestal 20.
// Physical source clock, I/O timing, ROM inference and DAC ownership still
// require board-specific synthesis and bench qualification.
module phase5_360_reference (
    input  wire       clk40,
    input  wire       reset,
    input  wire [7:0] raw_iq,
    output reg  [5:0] dac_code
);
    reg [4:0] phase_rom [0:255];
    reg [5:0] dac_rom [0:2047];
    initial begin
        $readmemh("hardware/phase5_360/phase5_256x5.mem", phase_rom);
        $readmemh("hardware/phase5_360/dac_2048x6.mem", dac_rom);
    end

    reg first_sample;
    reg previous_valid;
    reg [4:0] previous_phase;
    reg [4:0] middle_phase;
    wire [4:0] current_phase = phase_rom[raw_iq];

    wire [4:0] relative_middle = middle_phase - previous_phase;
    wire [4:0] endpoint = current_phase - previous_phase;
    wire [5:0] positive_limit = 6'd16 + {1'b0, endpoint};
    wire [4:0] negative_limit = endpoint - 5'd16;
    // e>=0: k=-1 for 16<=u<=16+e.
    // e<0:  k=+1 for 16+e<u<16.
    wire negative_winding = !endpoint[4] && relative_middle[4] &&
                            ({1'b0, relative_middle} <= positive_limit);
    wire positive_winding = endpoint[4] && !relative_middle[4] &&
                            (relative_middle > negative_limit);
    wire winding_flag = negative_winding || positive_winding;
    wire [10:0] dac_address = {winding_flag, previous_phase, current_phase};

    always @(posedge clk40) begin
        if (reset) begin
            first_sample <= 1'b1;
            previous_valid <= 1'b0;
            previous_phase <= 5'd0;
            middle_phase <= 5'd0;
            dac_code <= 6'd20;
        end else if (first_sample) begin
            middle_phase <= current_phase;
            first_sample <= 1'b0;
        end else begin
            // One lookup of a *computed* 11-bit address; no CPU sample work.
            dac_code <= previous_valid ? dac_rom[dac_address] : 6'd20;
            previous_phase <= current_phase;
            previous_valid <= 1'b1;
            first_sample <= 1'b1;
        end
    end
endmodule
