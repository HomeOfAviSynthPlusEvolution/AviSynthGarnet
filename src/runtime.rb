module AVS
  def self.function_exists?(name)
    call(:FunctionExists, name.to_s)
  end
  # Native signatures remain available through export; library authors normally
  # only need a readable schema and a block (or a block delegating to a class).
  def self.filter(name, args: {}, options: {}, &body)
    signature, wrapper = __schema(args, options, body)
    export(name, signature, &wrapper)
  end

  def self.function(args: {}, options: {}, returns: :any, &body)
    signature, wrapper = __schema(args, options, body)
    code = __type(returns)
    __function(signature, code, &wrapper)
  end

  def self.__type(type)
    types = {clip: 'c', bool: 'b', int: 'i', float: 'f', string: 's', func: 'n', any: '.'}
    code = types[type]
    raise ArgumentError, "unknown AVS type #{type}" unless code
    code
  end

  def self.__schema(args, options, body)
    raise ArgumentError, 'a block is required' unless body
    seen = {}
    signature = ''
    [args, options].each_with_index do |schema, kind|
      raise TypeError, 'parameter schema must be a Hash' unless schema.is_a?(Hash)
      schema.each do |key, type|
        text = key.to_s
        canonical = text.downcase
        raise ArgumentError, "duplicate parameter #{text}" if seen[canonical]
        seen[canonical] = true
        code = __type(type)
        signature += kind == 0 ? code : "[#{text}]#{code}"
      end
    end
    # Snapshot names: later mutation of the caller's schema must not change
    # dispatch while the host still uses the original signature.
    names = options.keys.map { |key| key.to_s.to_sym }
    required = args.size
    wrapper = ->(*values) do
      keywords = {}
      names.each_with_index do |key, i|
        value = values[required + i]
        keywords[key] = value unless value.nil?
      end
      body.call(*values[0, required], **keywords)
    end
    [signature, wrapper]
  end
end
