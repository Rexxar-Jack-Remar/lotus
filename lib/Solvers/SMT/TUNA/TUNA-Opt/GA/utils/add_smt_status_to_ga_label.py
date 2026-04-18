import csv
import os.path
import re

from utils.config import CONFIG

def extract_status_from_file(file_path):
    try:
        with open(file_path, 'r') as f:
            content = f.read()
            match = re.search(r'\(set-info\s+:status\s+(sat|unsat)\)', content)
            if match:
                return match.group(1)
            return 'unknown'
    except FileNotFoundError:
        return 'file_not_found'


def process_csv(input_csv, output_csv):
    # Read the input CSV
    with open(input_csv, 'r') as infile:
        reader = csv.DictReader(infile)
        fieldnames = reader.fieldnames + ['status']  # Add new column

        # Write to output CSV
        with open(output_csv, 'w', newline='') as outfile:
            writer = csv.DictWriter(outfile, fieldnames=fieldnames)
            writer.writeheader()

            for row in reader:
                file_path = row['file_relative_path']
                status = extract_status_from_file(os.path.join(CONFIG.dataset_dir, file_path))
                row['status'] = status
                writer.writerow(row)


if __name__ == '__main__':
    # Example usage
    input_csv = CONFIG.cvc5_all_ga_label_path  # Replace with your input CSV path
    output_csv = CONFIG.cvc5_all_ga_label_new_path  # Replace with your desired output path
    process_csv(input_csv, output_csv)