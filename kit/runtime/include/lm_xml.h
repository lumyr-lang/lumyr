#ifndef LM_XML_H
#define LM_XML_H

// xml(s) 内置函数：解析 XML 文本 → 值（DOM 式 map 结构）
// 返回 map { tagName, attributes, textContent, children }
//   tagName      string  标签名
//   attributes   map     属性键值（无属性时空 map）
//   textContent  string  该节点下所有直接文本/CDATA 拼接（无则为 ""）
//   children     array   子元素节点数组（无则为空数组）
//
// 忽略：XML 声明、DOCTYPE、注释
// 解析错误时返回 null（不抛异常，调用方可判空）
Value lumyr_xml_parse(const char* s);

#endif // LM_XML_H
