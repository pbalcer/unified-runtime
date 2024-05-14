#!/usr/bin/env python3

import os
import subprocess
import csv
import argparse
import io

# Function to run the benchmark with the given parameters and environment variables
def run_benchmark(directory, ioq, env_vars):
    # Set environment variables
    env = os.environ.copy()
    env.update(env_vars)

    # Construct the command
    command = [
        f"{directory}/api_overhead_benchmark_sycl",
        "--test=SubmitKernel",
        f"--Ioq={ioq}",
        "--DiscardEvents=0",
        "--MeasureCompletion=0",
        "--iterations=10000",
        "--Profiling=0",
        "--NumKernels=10",
        "--KernelExecTime=1",
        "--csv",
        "--noHeaders"
    ]
    
    # Run the command and capture the output
    result = subprocess.run(command, capture_output=True, text=True, env=env)
    
    # Return the output
    return result.stdout

# Function to parse the CSV output and extract the mean execution time
def parse_output(output):
    # Use StringIO to turn the string output into a file-like object for the csv reader
    csv_file = io.StringIO(output)
    reader = csv.reader(csv_file)
    
    # Skip the header row
    next(reader, None)
    
    # Read the second line which contains the data
    data_row = next(reader, None)
    if data_row is None:
        raise ValueError("Benchmark output does not contain data.")
    
    try:
        mean = float(data_row[1])  # Mean is the second value
        return mean
    except ValueError:
        raise ValueError(f"Could not convert mean execution time to float: '{data_row[1]}'")
    except IndexError:
        raise ValueError("Data row does not contain enough values.")

# Function to generate the mermaid bar chart script
def generate_mermaid_script(means, labels):
    max_mean = max(means)
    y_axis_scale = ((max_mean // 100) + 1) * 100  # Round up to the nearest multiple of 100
   
    # Format labels with double quotes
    formatted_labels = ', '.join(f'"{label}"' for label in labels)

    mermaid_script = f"""xychart-beta
    title "api_overhead_benchmark_sycl (lower is better)"
    x-axis [{formatted_labels}]
    y-axis "mean execution time per 10 kernels (in μs)" 0 --> {y_axis_scale}
    bar {means}
    """
    
    return mermaid_script

# Main function to run the benchmarks and generate the mermaid script
def main(directory):
    # Variants to run with corresponding x-axis labels
    variants = [
        (1, {'UR_L0_USE_IMMEDIATE_COMMANDLISTS': '0'}, "Batched In Order"),
        (0, {'UR_L0_USE_IMMEDIATE_COMMANDLISTS': '0'}, "Batched Out Of Order"),
        (1, {'UR_L0_USE_IMMEDIATE_COMMANDLISTS': '1'}, "Immediate In Order"),
        (0, {'UR_L0_USE_IMMEDIATE_COMMANDLISTS': '1'}, "Immediate Out Of Order"),
    ]
    
    # Run benchmarks and collect means and labels
    means = []
    labels = []
    for ioq, env_vars, label in variants:
        output = run_benchmark(directory, ioq, env_vars)
        mean = parse_output(output)
        means.append(mean)
        labels.append(label)
    
    # Generate mermaid script
    mermaid_script = generate_mermaid_script(means, labels)
    
    # Write mermaid script to a file
    with open('benchmark_chart.mmd', 'w') as file:
        file.write(mermaid_script)

    print("Mermaid script has been written to benchmark_chart.mmd")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Run benchmarks and generate a Mermaid bar chart script.')
    parser.add_argument('benchmark_directory', type=str, help='The directory where the benchmarks are located.')
    args = parser.parse_args()
    main(args.benchmark_directory)
