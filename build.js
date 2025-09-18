#!/usr/bin/env node
// @ts-check
const { Buffer } = require("buffer")
const fs = require("fs")
const path = require("path")

const BaseDir = '.' // __dirname
const Encoding = "utf8"

function GetFileName(file) {
  return path.join(BaseDir, "src", file)
}

/**
 * replace [start, end) with data, and data may be longer than the range
 * @param {Buffer} buffer source & dest buffer
 * @param {number} start 
 * @param {number} end 
 * @param {Buffer} src to replace with
 */
function BufferReplace(buffer, start, end, src) {
  if (end < start) throw new RangeError()
  const head = Buffer.copyBytesFrom(buffer, 0, start)
  const tail = Buffer.copyBytesFrom(buffer, end)
  return Buffer.concat([head, src, tail])
}

/**
 * find `macro` in `data` and replace it with `file` content
 * @param {Buffer} data 
 * @param {string} macro 
 * @param {string} file 
 * @returns 
 */
function ReplaceFile(data, macro, file) {
  const pos = data.indexOf(macro, 0, Encoding)
  if (pos === -1) return data
  const pos_end = pos + macro.length
  if (!fs.existsSync(file)) {
    console.log(`file not exist: ${file}`)
    return data
  }
  let content = fs.readFileSync(file)

  const inc_header = `#include "ipp_inc.h"`
  let pos_inc = content.indexOf(inc_header, 0, Encoding)
  if (pos_inc === 0) {
    pos_inc = inc_header.length
    while (pos_inc < content.length && content[pos_inc] === 10) { // '\n' -> 10
      pos_inc++
    }
    // console.log(`trim ${pos_inc} bytes from ${file}`)
    content = content.slice(pos_inc)
  } else if (pos_inc > 0) {
    console.log(`Should place "${inc_header}" at the top of file`)
  }

  console.log(`Replace [${pos}, ${pos_end}) with ${file} (${content.length})`)
  return BufferReplace(data, pos, pos_end, content)
}

function Build() {
  let src = fs.readFileSync(GetFileName('bttrack.cpp'))
  console.log(`bttrack.cpp length: ${src.length}`)

  const ipps = ["ipp_inc.ipp", "output.ipp", "slice.ipp", "utils.ipp"]
  for (const i of ipps) {
    src = ReplaceFile(src, `#include "${i}"`, GetFileName(i))
  }
  console.log(`bttrack.cpp length: ${src.length}`)

  const dst_path = path.join(BaseDir, 'bttrack.cpp')
  fs.writeFileSync(dst_path, src)
  console.log(`written to ${dst_path}`)
}

Build()
