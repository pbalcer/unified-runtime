#!/usr/bin/env python3

import os
import subprocess  # nosec B404
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
    result = subprocess.run(command, capture_output=True, text=True, env=env)  # nosec B603

    # Return the command and output
    return command, result.stdout

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

# Function to generate the markdown collapsible sections for each variant
def generate_markdown_details(variant_details):
    markdown_sections = []
    for label, command, env_vars, output in variant_details:
        env_vars_str = '\n'.join(f"{key}={value}" for key, value in env_vars.items())
        markdown_sections.append(
            f"### {label}\n"
            f"<details>\n"
            f"<summary>Click to expand</summary>\n\n"
            f"**Environment Variables:**\n"
            f"```\n{env_vars_str}\n```\n\n"
            f"**Command:**\n"
            f"```\n{' '.join(command)}\n```\n\n"
            f"**Output:**\n"
            f"```\n{output}\n```\n"
            f"</details>\n\n"
        )
    return "\n".join(markdown_sections)

# Function to generate the markdown with the Mermaid chart
def generate_markdown_with_mermaid_chart(mermaid_script, variant_details):
    markdown_content = (
        "# Benchmark Results\n\n"
        "## Chart\n\n"
        "```mermaid\n"
        f"{mermaid_script}\n"
        "```\n\n"
        "## Details\n\n"
        f"{generate_markdown_details(variant_details)}"
    )
    return markdown_content

# Main function to run the benchmarks and generate the markdown
def main(directory, additional_env_vars):
    # Variants to run with corresponding x-axis labels
    variants = [
        (1, {'UR_L0_USE_IMMEDIATE_COMMANDLISTS': '0'}, "Batched In Order"),
        (0, {'UR_L0_USE_IMMEDIATE_COMMANDLISTS': '0'}, "Batched Out Of Order"),
        (1, {'UR_L0_USE_IMMEDIATE_COMMANDLISTS': '1'}, "Immediate In Order"),
        (0, {'UR_L0_USE_IMMEDIATE_COMMANDLISTS': '1'}, "Immediate Out Of Order"),
    ]

    # Run benchmarks and collect means, labels, and variant details
    means = []
    labels = []
    variant_details = []  # Store label, command, and output for each variant
    for ioq, env_vars, label in variants:
        # Merge additional environment variables with the existing ones for this variant
        merged_env_vars = {**env_vars, **additional_env_vars}
        command, output = run_benchmark(directory, ioq, merged_env_vars)
        mean = parse_output(output)
        means.append(mean)
        labels.append(label)
        variant_details.append((label, command, merged_env_vars, output))

    # Generate mermaid script
    mermaid_script = generate_mermaid_script(means, labels)

    # Generate markdown content
    markdown_content = generate_markdown_with_mermaid_chart(mermaid_script, variant_details)

    # Write markdown content to a file
    with open('benchmark_results.md', 'w') as file:
        file.write(markdown_content)

    print("Markdown with benchmark results has been written to benchmark_results.md")

def validate_and_parse_env_args(env_args):
    env_vars = {}
    for arg in env_args:
        if '=' not in arg:
            raise ValueError(f"Environment variable argument '{arg}' is not in the form Variable=Value.")
        key, value = arg.split('=', 1)
        env_vars[key] = value
    return env_vars

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Run benchmarks and generate a Mermaid bar chart script.')
    parser.add_argument('benchmark_directory', type=str, help='The directory where the benchmarks are located.')
    parser.add_argument("--env", type=str, help='Add env variable', action="append", default=[])
    args = parser.parse_args()

    # Validate and parse additional environment variable arguments
    additional_env_vars = validate_and_parse_env_args(args.env)

    main(args.benchmark_directory, additional_env_vars)
