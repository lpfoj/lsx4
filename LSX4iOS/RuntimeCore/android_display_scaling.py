"""Mechanical native adaptation of Android's pure presentation geometry.

The complete policy body stays source-derived. Unsupported upstream Java syntax
fails C++ compilation/conformance instead of silently substituting a new policy.
"""
from pathlib import Path
import re

def native_header(source: str) -> str:
    source=re.sub(r'^package app\.lsx4\.android;\s*','',source,count=1)
    assert source.count('final class DisplayScaling {')==1
    source=source.replace('final class DisplayScaling {','struct AndroidDisplayScaling {',1)
    source=source.replace('static final int[] ORDER = {','inline static constexpr int ORDER[] = {')
    source=source.replace('static final int ','static constexpr int ')
    source=source.replace('static final class Dimensions {','struct Dimensions {')
    source=source.replace('final int width;','const int width;').replace('final int height;','const int height;')
    constructor='''Dimensions(int width, int height) {
            this.width = width;
            this.height = height;
        }'''
    assert source.count(constructor)==1
    source=source.replace(constructor,'Dimensions(int width_, int height_) : width(width_), height(height_) {}')
    source=source.replace('private DisplayScaling() {\n    }','AndroidDisplayScaling() = delete;')
    source=source.replace('private static ','static ').replace('new Dimensions(', 'Dimensions(')
    source=re.sub(r'\bboolean\b','bool',source)
    source=source.replace('ORDER.length','static_cast<int>(std::size(ORDER))')
    for name in ['max','min','floor']:source=source.replace('Math.'+name,'std::'+name)
    source=source.replace('Math.round(value)','static_cast<int>(std::lround(value))')
    # Java nested/top-level class terminators need native semicolons.
    source=source.replace('height(height_) {}\n    }','height(height_) {}\n    };')
    source=source.rstrip()+';\n'
    return '#pragma once\n#include <algorithm>\n#include <cmath>\n#include <iterator>\n// Generated from complete Android DisplayScaling.java policy.\n'+source
