import json
import Simulator

# 1. Save your text snippet above into a file named "sample.txt"
input_file = "Sample Testcase.txt"
output_file = "cases.jsonl"

# 2. Simulator.py has a built-in function to split the trace from the expected output
trace, expected = Simulator._split_sample(input_file)

# 3. Rebuild the case dictionary from the trace
case_dict = Simulator.case_from_transcript(trace, profile="sample")

# 4. Write it out as a JSON-lines file
with open(output_file, "w") as f:
    f.write(json.dumps(case_dict) + "\n")

print(f"Successfully converted and saved to {output_file}")