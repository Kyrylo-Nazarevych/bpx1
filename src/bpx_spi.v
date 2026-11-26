// bpx_spi - control port: SPI mode 0 slave, MSB first, 16-bit registers (docs/SPEC.md section 4).
//
//   CSn low | byte: {write, 0, addr[5:0]} | 16-bit word | 16-bit word | ...     address + 1 after every word
//
// SCK, MOSI and CSn arrive already synchronised to clk (two flops each) and are only sampled here: SCK is data,
// not a clock.  Each SCK phase must last at least four clk periods (f_SCK <= f_clk / 8).  MISO changes after the
// falling SCK edge and is 0 while CSn is high.
`default_nettype none

module bpx_spi (
    input  wire        clk,
    input  wire        rst,
    input  wire        sck,
    input  wire        mosi,
    input  wire        csn,
    output reg         miso,
    output reg  [5:0]  addr,
    output reg         we,      // one clk: write wdata to addr
    output reg         re,      // one clk: the word at addr has just been taken for transmission
    output reg  [15:0] wdata,
    input  wire [15:0] rdata
);
  reg        sck_q, have_cmd, rw, inc;
  reg [3:0]  cnt;
  reg [15:0] sh;
  wire       rise = sck & ~sck_q;
  wire       fall = ~sck & sck_q;

  always @(posedge clk) begin
    if (rst) begin
      sck_q    <= 1'b0;
      have_cmd <= 1'b0;
      rw       <= 1'b0;
      inc      <= 1'b0;
      cnt      <= 4'd0;
      sh       <= 16'd0;
      miso     <= 1'b0;
      addr     <= 6'd0;
      we       <= 1'b0;
      re       <= 1'b0;
      wdata    <= 16'd0;
    end else begin
      sck_q <= sck;
      we    <= 1'b0;
      re    <= 1'b0;
      inc   <= we | re;                    // the address moves on one clk after the access
      if (inc) addr <= addr + 6'd1;
      if (csn) begin
        have_cmd <= 1'b0;
        cnt      <= 4'd0;
        miso     <= 1'b0;
      end else begin
        if (rise) begin
          if (!have_cmd || rw) sh <= {sh[14:0], mosi};   // while reading, sh is the transmit register
          if (!have_cmd) begin
            if (cnt == 4'd7) begin
              have_cmd <= 1'b1;
              rw       <= sh[6];
              addr     <= {sh[4:0], mosi};
              cnt      <= 4'd0;
            end else cnt <= cnt + 4'd1;
          end else begin
            cnt <= cnt + 4'd1;             // wraps at 16
            if (cnt == 4'd15 && rw) begin
              wdata <= {sh[14:0], mosi};
              we    <= 1'b1;
            end
          end
        end
        if (fall && have_cmd && !rw) begin
          if (cnt == 4'd0) begin           // start of a word: take it from the register file
            miso <= rdata[15];
            sh   <= {rdata[14:0], 1'b0};
            re   <= 1'b1;
          end else begin
            miso <= sh[15];
            sh   <= {sh[14:0], 1'b0};
          end
        end
      end
    end
  end
endmodule
