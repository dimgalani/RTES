using Plots
using PlotThemes
using StatsBase
using Printf

theme(:rose_pine_dawn::Symbol;)

# Function to read the file TradeDelays.txt and extract the delays
function read_trade_delays(TradeFilename)
    server_delays = []
    trade_writing_delays = []
    
    open(TradeFilename, "r") do file
        for line in eachline(file)
            if occursin("Server delay until receiving", line)
                server_delay = parse(Int, match(r"Server delay until receiving: (\d+)ms", line).captures[1])
                writing_delay = parse(Int, match(r"Writing delay: (\d+)ms", line).captures[1])
                push!(server_delays, server_delay)
                push!(trade_writing_delays, writing_delay)
            end
        end
    end
    return server_delays, trade_writing_delays
end

# Function to read the file OnMinuteDelays.txt and extract the delays
function read_on_minute_delays(OnMinuteFilename)
    candlestick_delays = []
    moving_avg_delays = []
    
    open(OnMinuteFilename, "r") do file
        for line in eachline(file)
            if occursin("Candlestick delay:", line)
                candle_delay = parse(Int, match(r"Candlestick delay: (\d+)ms", line).captures[1])
                avg_delay = parse(Int, match(r"Moving average delay: (\d+)ms", line).captures[1])
                push!(candlestick_delays, candle_delay)
                push!(moving_avg_delays, avg_delay)
            end
        end
    end
    return candlestick_delays, moving_avg_delays
end

# Function to read the file and extract the first two timestamps
function read_timestamps(timestampsFilename)
    should_have_done = []
    actually_started = []
    candlestick_written = []
    fifteen = []
    
    open(timestampsFilename, "r") do file
        for line in eachline(file)
            # Extract the two timestamps using regular expressions
            timestamp_should_done = parse(Int, match(r"Timestamp when the calculation should have been done: (\d+)", line).captures[1])
            timestamp_actually_started = parse(Int, match(r"Timestamp when the calculation actually started: (\d+)", line).captures[1])
            timestamp_candlestick_written = parse(Int, match(r"Timestamp when the candlestick was written: (\d+)", line).captures[1])
            timestamp_fifteen = parse(Int, match(r"Timestamp when the moving average was written: (\d+)", line).captures[1])
            push!(should_have_done, timestamp_should_done)
            push!(actually_started, timestamp_actually_started)
            push!(candlestick_written, timestamp_candlestick_written)
            push!(fifteen, timestamp_fifteen)
        end
    end
    return should_have_done, actually_started, candlestick_written, fifteen
end

# Load the files
TradeFilename = "TradeDelays.txt"
OnMinuteFilename = "OnMinuteDelays.txt"
timestampsFilename = "OnMinuteTimestamps.txt"

# Call the functions to read the numbers
server_delays, trade_writing_delays = read_trade_delays(TradeFilename)
candlestick_delays, moving_avg_delays = read_on_minute_delays(OnMinuteFilename)
should_have_done, actually_started, candlestick_written, fifteen = read_timestamps(timestampsFilename)

# Create the plots
# 1. Plot histogram for server delays
hist = histogram(server_delays, bins=50000, xlims=(0,9000), 
          xlabel="Server Delay (ms)", ylabel="Number of Samples", 
          title="Distribution of Server Delay", legend=false, fill=true)
savefig(hist, "server_delays.png")
display(hist)

# 2. Plot bar count chart for writing delays
bar_plot_writing = bar(countmap(trade_writing_delays), xlims=(0,5), xlabel="Writing Delay (ms)", ylabel="Number of Samples", title="Distribution of Writing Delay of Trades", legend=false)
savefig(bar_plot_writing, "trade_writing_delays.png")
display(bar_plot_writing)

# 3. Plot bar count chart for candlestick delays
bar_plot_candle = bar(countmap(candlestick_delays), xlims=(0,10), xlabel="Candlestick Delay (ms)", ylabel="Number of Samples", title="Distribution of Candlestick Delay", legend=false)
savefig(bar_plot_candle, "candlestick_delays.png")
display(bar_plot_candle)

# 4. Plot bar count chart for moving average delays
bar_plot_avg = bar(countmap(moving_avg_delays), xlims=(0,10), xlabel="Moving Average Delay (ms)", ylabel="Number of Samples", title="Distribution of Moving Average Delay", legend=false)
savefig(bar_plot_avg, "moving_avg_delays.png")
display(bar_plot_avg)

# 5. Create the scatter plot from the timestamps
# Calculate the differences between consecutive elements in the actually_started array
differences = [should_have_done[i+1] - should_have_done[i] for i in 1:length(should_have_done)-1]
print(differences)
# Plot the differences
diff_plot = scatter(differences, xlabel="Index", ylabel="Difference (ms)", 
                 title="Differences Between Consecutive Actually Started Timestamps", legend=false)
savefig(diff_plot, "differences.png")
display(diff_plot)

formatter = x -> @sprintf("%.0f", x)
sct = scatter(should_have_done, actually_started, 
        xlabel="Timestamp (Should Have Been Done)", ylabel="Timestamp (Actually Started)", 
        title="Timestamp Scatter Plot", legend=false, xformatter=formatter, yformatter=formatter, marker=:star)
savefig(sct, "timestamps.png")
display(sct)