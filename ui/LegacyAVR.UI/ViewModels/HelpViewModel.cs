using System;
using System.Collections.ObjectModel;
using System.Linq;

namespace LegacyAVR.UI.ViewModels;

/// <summary>Searchable Settings Guide: filters by title, body text, category and keywords.</summary>
public sealed class HelpViewModel : ObservableObject
{
    private string _search = string.Empty;
    private string _category = AllCategories;
    public const string AllCategories = "All topics";

    public HelpViewModel()
    {
        Categories = new ObservableCollection<string>(
            new[] { AllCategories }.Concat(HelpContent.Entries.Select(e => e.Category).Distinct()));
        Apply();
    }

    public ObservableCollection<string> Categories { get; }
    public ObservableCollection<HelpEntry> Results { get; } = new();

    public string SearchText
    {
        get => _search;
        set { if (Set(ref _search, value)) Apply(); }
    }

    public string SelectedCategory
    {
        get => _category;
        set { if (Set(ref _category, value ?? AllCategories)) Apply(); }
    }

    public string ResultCountText => Results.Count == 1 ? "1 topic" : $"{Results.Count} topics";

    private void Apply()
    {
        var words = _search.Split(' ', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        Results.Clear();
        foreach (var e in HelpContent.Entries)
        {
            if (_category != AllCategories && e.Category != _category) continue;
            var hay = string.Join(' ', e.Category, e.Title, e.WhatItDoes, e.Tradeoffs, e.StartingPoint, e.Keywords);
            if (words.All(w => hay.Contains(w, StringComparison.OrdinalIgnoreCase))) Results.Add(e);
        }
        OnPropertyChanged(nameof(ResultCountText));
    }
}
