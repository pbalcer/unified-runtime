#!/usr/bin/env python3

import os
import subprocess  # nosec B404
import csv
import argparse
import io
import json
from pathlib import Path

# Function to run the benchmark with the given parameters and environment variables
def run_benchmark(directory, ioq, env_vars):
    env = os.environ.copy()
    env.update(env_vars)
    command = [
        f"{directory}/api_overhead_benchmark_l0",
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
    result = subprocess.run(command, capture_output=True, text=True, env=env)  # nosec B603
    return command, result.stdout

# Function to parse the CSV output and extract the mean execution time
def parse_output(output):
    # Use StringIO to turn the string output into a file-like object for the csv reader
    csv_file = io.StringIO(output)
    reader = csv.reader(csv_file)

    # Skip the header row
    next(reader, None)
    data_row = next(reader, None)
    if data_row is None:
        raise ValueError("Benchmark output does not contain data.")
    try:
        mean = float(data_row[1]) # Mean is the second value
        return mean
    except ValueError:
        raise ValueError(f"Could not convert mean execution time to float: '{data_row[1]}'")
    except IndexError:
        raise ValueError("Data row does not contain enough values.")

# Function to generate the mermaid bar chart script
def generate_mermaid_script(means, labels, comparison_data):
    max_mean = max(means)
    if comparison_data:
        max_mean = max(max_mean, max(comparison_data.values()))
    y_axis_scale = ((max_mean // 100) + 1) * 100

    # Format labels with double quotes
    formatted_labels = ', '.join(f'"{label}"' for label in labels)

    mermaid_script = f"""xychart-beta
title "api_overhead_benchmark_sycl (lower is better)"
x-axis [{formatted_labels}]
y-axis "mean execution time per 10 kernels (in μs)" 0 --> {y_axis_scale}
bar {means}
"""

    if comparison_data:
        comparison_means = [comparison_data.get(label, 0) for label in labels]
        mermaid_script += f"line {comparison_means}\n"

    return mermaid_script

# Function to generate the markdown collapsible sections for each variant
def generate_markdown_details(variant_details):
    markdown_sections = []
    for label, command, env_vars, output in variant_details:
        env_vars_str = '\n'.join(f"{key}={value}" for key, value in env_vars.items())
        markdown_sections.append(f"""### {label}
<details>
<summary>Click to expand</summary>

#### Environment Variables:
{env_vars_str}

#### Command:
{' '.join(command)}

#### Output:
{output}

</details>
""")
    return "\n".join(markdown_sections)

def compare_note(pct_differences):
    content = ""
    if pct_differences:
        content += "Comparison with previous data:\n\n"
        for label, pct_difference in pct_differences.items():
            content += f"- {label}: {pct_difference:+.2f}%\n"
    else:
        content += "Comparison data not found. No comparison performed."

    return f"""
## Comparison\n
{content}
    """

# Function to generate the full markdown
def generate_markdown_with_mermaid_chart(mermaid_script, variant_details, comparison_data):
    return f"""
# Benchmark Results
## Chart
{mermaid_script}
{compare_note(comparison_data)}
## Details
{generate_markdown_details(variant_details)}
"""

def save_benchmark_results(save_name, benchmark_data):
    benchmarks_dir = Path.home() / 'benchmarks'
    benchmarks_dir.mkdir(exist_ok=True)
    file_path = benchmarks_dir / f"{save_name}.json"
    with file_path.open('w') as file:
        json.dump(benchmark_data, file, indent=4)
    print(f"Benchmark results saved to {file_path}")

def load_benchmark_results(compare_name):
    benchmarks_dir = Path.home() / 'benchmarks'
    file_path = benchmarks_dir / f"{compare_name}.json"
    if file_path.exists():
        with file_path.open('r') as file:
            return json.load(file)
    else:
        return None

def compare_pct(benchmark_data, comparison_data):
    if comparison_data is None:
        return None

    pct_differences = {}
    for label, benchmark_value in benchmark_data.items():
        comparison_value = comparison_data.get(label)
        if comparison_value is not None:
            pct_difference = ((benchmark_value - comparison_value) / comparison_value) * 100
            pct_differences[label] = pct_difference
    return pct_differences

def main(directory, additional_env_vars, save_name=None, compare_name=None):
    variants = [
        (1, {'UR_L0_USE_IMMEDIATE_COMMANDLISTS': '0'}, "Batched In Order"),
        (0, {'UR_L0_USE_IMMEDIATE_COMMANDLISTS': '0'}, "Batched Out Of Order"),
        (1, {'UR_L0_USE_IMMEDIATE_COMMANDLISTS': '1'}, "Immediate In Order"),
        (0, {'UR_L0_USE_IMMEDIATE_COMMANDLISTS': '1'}, "Immediate Out Of Order"),
    ]

    # Run benchmarks and collect means, labels, and variant details
    means = []
    labels = []
    variant_details = []
    for ioq, env_vars, label in variants:
        merged_env_vars = {**env_vars, **additional_env_vars}
        command, output = run_benchmark(directory, ioq, merged_env_vars)
        mean = parse_output(output)
        means.append(mean)
        labels.append(label)
        variant_details.append((label, command, merged_env_vars, output))

    benchmark_data = {label: mean for label, mean in zip(labels, means)}

    if save_name:
        save_benchmark_results(save_name, benchmark_data)

    comparison_data = None
    if compare_name:
        comparison_data = load_benchmark_results(compare_name)

    mermaid_script = generate_mermaid_script(means, labels, comparison_data)

    comparison = compare_pct(benchmark_data, comparison_data)
    markdown_content = generate_markdown_with_mermaid_chart(mermaid_script, variant_details, comparison)

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
    parser.add_argument("--env", type=str, help='Use env variable for a benchmark run.', action="append", default=[])
    parser.add_argument("--save", type=str, help='Save the results for comparison under a specified name.')
    parser.add_argument("--compare", type=str, help='Compare results against previously saved data.')

    args = parser.parse_args()

    additional_env_vars = validate_and_parse_env_args(args.env)

    main(args.benchmark_directory, additional_env_vars, save_name=args.save, compare_name=args.compare)
