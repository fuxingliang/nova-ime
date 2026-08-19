using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

namespace PinyinPlus.Server;

/// <summary>
/// 符号表数据：解析 tools/data/symbols.txt（格式：拼音\t符号[\t简拼]，# ---- 分类名 ---- 分隔）。
/// 供符号面板分类浏览；点击符号经 Server→DLL(type 7) 插入编辑器。
/// </summary>
public static class SymbolData
{
    public record SymbolCategory(string Name, List<string> Symbols);

    public static IReadOnlyList<SymbolCategory> Load()
    {
        var result = new List<SymbolCategory>();
        try
        {
            string? path = LocateSymbolsFile();
            if (path is null)
                return result;

            string currentCat = "未分类";
            foreach (string line in File.ReadLines(path, Encoding.UTF8))
            {
                string t = line.Trim();
                if (t.Length == 0)
                    continue;
                if (t[0] == '#')
                {
                    // 分类标记：# ---- 名称 ----
                    var m = Regex.Match(t, @"#\s*----\s*(.+?)\s*----");
                    if (m.Success)
                        currentCat = m.Groups[1].Value.Trim();
                    continue;
                }
                int firstTab = t.IndexOf('\t');
                if (firstTab < 0)
                    continue;
                // 行格式：pinyin \t symbol [\t initial] —— 只取第 2 列符号，简拼列必须排除
                string symbol;
                int secondTab = t.IndexOf('\t', firstTab + 1);
                symbol = secondTab < 0
                    ? t.Substring(firstTab + 1).Trim()
                    : t.Substring(firstTab + 1, secondTab - firstTab - 1).Trim();
                if (symbol.Length == 0)
                    continue;

                var cat = result.Find(c => c.Name == currentCat);
                if (cat is null)
                {
                    cat = new SymbolCategory(currentCat, new List<string>());
                    result.Add(cat);
                }
                cat.Symbols.Add(symbol);
            }
        }
        catch
        {
            // 符号表缺失/损坏时返回空面板，不影响候选窗
        }
        return result;
    }

    /// <summary>定位 symbols.txt：安装目录根下（安装包化：安装目录 = 含词库/符号表的目录）。</summary>
    private static string? LocateSymbolsFile()
    {
        string direct = Path.Combine(Paths.InstallDir, "symbols.txt");
        return File.Exists(direct) ? direct : null;
    }
}
