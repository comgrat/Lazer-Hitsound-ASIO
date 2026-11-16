using System.Collections.Concurrent;
using System.Reflection;
using System.Runtime.CompilerServices;

// Cache metadata by runtime type, never object values or mutable graph edges.
// Weak keys let a collectible client/type disappear with its load context.
internal static class AudioReflection
{
    const BindingFlags Instance = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
    sealed record FieldSlot(FieldInfo? Member);
    sealed record PropertySlot(PropertyInfo? Member);
    sealed class Members
    {
        public readonly ConcurrentDictionary<string, FieldSlot> Fields = new(StringComparer.Ordinal);
        public readonly ConcurrentDictionary<string, PropertySlot> Properties = new(StringComparer.Ordinal);
    }
    sealed record WeakReader(Func<object, object?> Read);
    static readonly ConditionalWeakTable<Type, Members> members = new();
    static readonly ConditionalWeakTable<Type, WeakReader> weakReaders = new();

    public static object? Field(object instance, string name)
    {
        var type = instance.GetType();
        var cache = members.GetValue(type, static _ => new Members());
        if (!cache.Fields.TryGetValue(name, out var slot))
        {
            FieldInfo? field = null;
            for (Type? cursor = type; cursor != null && field == null; cursor = cursor.BaseType)
                field = cursor.GetField(name, Instance | BindingFlags.DeclaredOnly);
            slot = cache.Fields.GetOrAdd(name, new FieldSlot(field));
        }
        return slot.Member?.GetValue(instance);
    }

    public static object? Property(object instance, string name)
    {
        var type = instance.GetType();
        var cache = members.GetValue(type, static _ => new Members());
        if (!cache.Properties.TryGetValue(name, out var slot))
            slot = cache.Properties.GetOrAdd(name, new PropertySlot(type.GetProperty(name, Instance)));
        return slot.Member?.GetValue(instance);
    }

    public static object? WeakTarget(object weak)
        => weakReaders.GetValue(weak.GetType(), CreateWeakReader).Read(weak);

    static WeakReader CreateWeakReader(Type type)
    {
        if (!type.IsGenericType || type.GetGenericTypeDefinition() != typeof(WeakReference<>))
            throw new NotSupportedException("Unexpected audio source weak-reference type: " + type);
        var method = typeof(AudioReflection).GetMethod(nameof(ReadWeak), BindingFlags.Static | BindingFlags.NonPublic)!
            .MakeGenericMethod(type.GetGenericArguments()[0]);
        return new WeakReader(method.CreateDelegate<Func<object, object?>>());
    }

    static object? ReadWeak<T>(object weak) where T : class
        => ((WeakReference<T>)weak).TryGetTarget(out var target) ? target : null;
}
