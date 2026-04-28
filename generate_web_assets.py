#!/usr/bin/env python3
"""
Generate web_assets.h from web/ source files.
Converts HTML, CSS, and JS into C string literals.
"""

import os
import sys

def escape_c_string(content):
    """Escape content for C string literal."""
    # Replace special characters
    content = content.replace('\\', '\\\\')  # Backslash first
    content = content.replace('"', '\\"')    # Double quotes
    content = content.replace('\r\n', '\n')  # Normalize line endings
    
    # Split into lines and add newline markers
    lines = content.split('\n')
    escaped_lines = []
    for line in lines:
        escaped_lines.append(line)
    
    return '\\n"\n"'.join(escaped_lines)

def read_file(filepath):
    """Read file content."""
    with open(filepath, 'r', encoding='utf-8') as f:
        return f.read()

def generate_c_array(varname, content):
    """Generate C string array declaration."""
    escaped = escape_c_string(content)
    return f'static const char {varname}[] = \n"{escaped}\\n";\n\n'

def main():
    base_dir = os.path.dirname(os.path.abspath(__file__))
    web_dir = os.path.join(base_dir, 'web')
    output_file = os.path.join(base_dir, 'src', 'web_assets.h')
    
    if not os.path.isdir(web_dir):
        print(f"Error: web/ directory not found at {web_dir}")
        sys.exit(1)
    
    # Read files
    try:
        index_html = read_file(os.path.join(web_dir, 'index.html'))
        config_html = read_file(os.path.join(web_dir, 'config.html'))
        banks_html = read_file(os.path.join(web_dir, 'banks.html'))
        diag_html = read_file(os.path.join(web_dir, 'diag.html'))
        styles_css = read_file(os.path.join(web_dir, 'styles.css'))
        app_js = read_file(os.path.join(web_dir, 'app.js'))
    except FileNotFoundError as e:
        print(f"Error: Could not read required file: {e}")
        sys.exit(1)
    
    # Generate C header
    c_code = '#pragma once\n\n'
    c_code += generate_c_array('web_index_html', index_html)
    c_code += generate_c_array('web_config_html', config_html)
    c_code += generate_c_array('web_banks_html', banks_html)
    c_code += generate_c_array('web_diag_html', diag_html)
    c_code += generate_c_array('web_styles_css', styles_css)
    c_code += generate_c_array('web_app_js', app_js)
    
    # Write output
    with open(output_file, 'w', encoding='utf-8') as f:
        f.write(c_code)
    
    print(f"✓ Generated {output_file}")
    print(f"  - web_index_html ({len(index_html)} bytes)")
    print(f"  - web_config_html ({len(config_html)} bytes)")
    print(f"  - web_banks_html ({len(banks_html)} bytes)")
    print(f"  - web_diag_html ({len(diag_html)} bytes)")
    print(f"  - web_styles_css ({len(styles_css)} bytes)")
    print(f"  - web_app_js ({len(app_js)} bytes)")

if __name__ == '__main__':
    main()
